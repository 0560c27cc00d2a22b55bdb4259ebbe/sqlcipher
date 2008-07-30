/* 
** SQLite Cipher
** crypto.c developed by Stephen Lombardo (Zetetic LLC) 
** sjlombardo at zetetic dot net
** http://zetetic.net
** 
** July 30, 2008
**
** This code is released under the same public domain terms as SQLite itself.
**
** THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
** IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
** FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
** AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
** LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
** OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
** THE SOFTWARE.
**  
*/
/* BEGIN CRYPTO */
#if defined(SQLITE_HAS_CODEC)

#include <assert.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include "sqliteInt.h"
#include "btreeInt.h"
#include "crypto.h"

typedef struct {
  int key_sz;
  Btree *pBt;
  void *key;
  void *rand;
  char *pKey;
  void *buffer;
} codec_ctx;

static int codec_page_hash(Pgno pgno, void *in, int inLen, void *out, int *outLen) {
  EVP_MD_CTX mdctx;
  unsigned int md_sz;
  unsigned char md_value[EVP_MAX_MD_SIZE];

  EVP_MD_CTX_init(&mdctx);
  EVP_DigestInit_ex(&mdctx, DIGEST, NULL);
    
  EVP_DigestUpdate(&mdctx, in, inLen); /* add random "salt" data to hash*/
  EVP_DigestUpdate(&mdctx, &pgno, sizeof(pgno));
  
  EVP_DigestFinal_ex(&mdctx, md_value, &md_sz);
  
  memcpy(out, md_value, md_sz);
    
  EVP_MD_CTX_cleanup(&mdctx);
  memset(md_value, 0, md_sz);
  *outLen =  md_sz;
}

static int codec_passphrase_hash(void *in, int inLen, void *out, int *outLen) {
  EVP_MD_CTX mdctx;
  unsigned int md_sz;
  unsigned char md_value[EVP_MAX_MD_SIZE];
  
  EVP_MD_CTX_init(&mdctx);
  EVP_DigestInit_ex(&mdctx, DIGEST, NULL);
  EVP_DigestUpdate(&mdctx, in, inLen);
  EVP_DigestFinal_ex(&mdctx, md_value, &md_sz);
  memcpy(out, md_value, md_sz);
  EVP_MD_CTX_cleanup(&mdctx);
  memset(md_value, 0, md_sz);
  *outLen = md_sz;
}

/*
 * ctx - codec context
 * pgno - page number in database
 * size - size in bytes of input and output buffers
 * mode - 1 to encrypt, 0 to decrypt
 * in - pointer to input bytes
 * out - pouter to output bytes
 */
static int codec_cipher(codec_ctx *ctx, Pgno pgno, int mode, int size, void *in, void *out) {
  EVP_CIPHER_CTX ectx;
  unsigned char iv[EVP_MAX_MD_SIZE];
  int iv_sz, tmp_csz, csz;
  
  /* the initilization vector is created from the hash of the
     16 byte database random salt and the page number. This will
     ensure that each page in the database has a unique initialization
     vector */
  codec_page_hash(pgno, ctx->rand, 16, iv, &iv_sz);
  
  EVP_CipherInit(&ectx, CIPHER, NULL, NULL, mode);
  EVP_CIPHER_CTX_set_padding(&ectx, 0);
  EVP_CipherInit(&ectx, NULL, ctx->key, iv, mode);
  EVP_CipherUpdate(&ectx, out, &tmp_csz, in, size);
  csz = tmp_csz;  
  out += tmp_csz;
  EVP_CipherFinal(&ectx, out, &tmp_csz);
  csz += tmp_csz;
  EVP_CIPHER_CTX_cleanup(&ectx);
  assert(size == csz);
}

/*
 * sqlite3Codec can be called in multiple modes.
 * encrypt mode - expected to return a pointer to the 
 *   encrypted data without altering pData.
 * decrypt mode - expected to return a pointer to pData, with
 *   the data decrypted in the input buffer
 */
void* sqlite3Codec(void *iCtx, void *pData, Pgno pgno, int mode) {
  int emode;
  codec_ctx *ctx = (codec_ctx *) iCtx;
  int pg_sz = sqlite3BtreeGetPageSize(ctx->pBt);
 
  switch(mode) {
    case 0: /* decrypt */
    case 2:
    case 3:
      emode = 0;
      break;
    case 6: /* encrypt */
    case 7:
      emode = 1;
      break;
    default:
      return pData;
      break;
  }

  if(pgno == 1 ) { 
    /* duplicate first 24 bytes of page 1 exactly to include random header data and page size */
    memcpy(ctx->buffer, pData, HDR_SZ); 

    /* if this is a read & decrypt operation on the first page then copy the 
       first 16 bytes off the page into the context's random salt buffer
       */
    if(emode) {
      memcpy(ctx->buffer, ctx->rand, 16);
    } else {
      memcpy(ctx->rand, pData, 16);
      memcpy(ctx->buffer, SQLITE_FILE_HEADER, 16);
    }
    
    /* adjust starting pointers in data page for header offset */
    codec_cipher(ctx, pgno, emode, pg_sz - HDR_SZ, &pData[HDR_SZ], &ctx->buffer[HDR_SZ]);
  } else {
    codec_cipher(ctx, pgno, emode, pg_sz, pData, ctx->buffer);
  }
  
  if(emode) {
    return ctx->buffer; /* return persistent buffer data, pData remains intact */
  } else {
    memcpy(pData, ctx->buffer, pg_sz); /* copy buffer data back to pData and return */
    return pData;
  }
}

int sqlite3CodecAttach(sqlite3* db, int nDb, const void *zKey, int nKey) {
  void *keyd;
  int len;
  char hout[1024];
  struct Db *pDb = &db->aDb[nDb];
  
  if(nKey && zKey &&  pDb->pBt) {
    codec_ctx *ctx;
    int rc;
    MemPage *pPage1;

    ctx = sqlite3_malloc(sizeof(codec_ctx));
    
    ctx->pBt = pDb->pBt; /* assign pointer to database btree structure */
    
    /* assign random salt data. This will be written to the first page
       if this is a new database file. If an existing database file is
       attached this will just be overwritten when the first page is
       read from disk */
    ctx->rand = sqlite3_malloc(16);
    RAND_pseudo_bytes(ctx->rand, 16);
    
    
    /* pre-allocate a page buffer of PageSize bytes. This will
       be used as a persistent buffer for encryption and decryption 
       operations to avoid overhead of multiple memory allocations*/
    ctx->buffer = sqlite3_malloc(sqlite3BtreeGetPageSize(ctx->pBt));
    
    ctx->key_sz = EVP_CIPHER_key_length(CIPHER);
    
    /* if key string starts with x' then assume this is a blob literal key*/
    if(sqlite3StrNICmp(zKey,"x'", 2) == 0) { 
      int n = nKey - 3; /* adjust for leading x' and tailing ' */
      const char *z = zKey + 2; /* adjust lead offset of x' */
      
      assert((n/2) == ctx->key_sz); /* keylength after hex decode should equal cipher key length */
      ctx->key = sqlite3HexToBlob(db, z, n);
    
    /* otherwise the key is provided as a string so hash it to get key data */
    } else {
      int key_sz;
      ctx->key = sqlite3_malloc(ctx->key_sz);
      codec_passphrase_hash(zKey, nKey, ctx->key, &key_sz);
      assert(key_sz == ctx->key_sz);
    } 
    
    sqlite3pager_set_codec(sqlite3BtreePager(pDb->pBt), sqlite3Codec, (void *) ctx);
  }
}

void sqlite3_activate_see(const char* in) {
  /* do nothing, security enhancements are always active */
}

int sqlite3_key(sqlite3 *db, const void *pKey, int nKey) {
  if(db) {
    int i;
    for(i=0; i<db->nDb; i++){
      sqlite3CodecAttach(db, i, pKey, nKey);
    }
  }
}

/* FIXME - this should change the password for the database! */
int sqlite3_rekey(sqlite3 *db, const void *pKey, int nKey) {
  printf("sorry sqlite3_rekey is not implemented yet\n");
  sqlite3_key(db, pKey, nKey);
}

void sqlite3CodecGetKey(sqlite3* db, int nDb, void **zKey, int *nKey) {
  extern int sqlite3pager_get_codec(Pager *pPager, void * ctx);
  codec_ctx *ctx;
  struct Db *pDb = &db->aDb[nDb];
  
  if( pDb->pBt ) {
    sqlite3pager_get_codec(pDb->pBt->pBt->pPager, (void **) &ctx);
    if(ctx) {
      *zKey = ctx->key;
      *nKey = ctx->key_sz;
    } else {
      *zKey = 0;
      *nKey = 0;  
    }
  }
  
}


#endif
/* END CRYPTO */
