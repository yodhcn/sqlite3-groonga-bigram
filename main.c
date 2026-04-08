#include <groonga.h>
#include <stdlib.h>
#include <inttypes.h>
#include <sqlite3ext.h> /* Do not use <sqlite3.h>! */

SQLITE_EXTENSION_INIT1

static int groonga_initialized = 0;

typedef struct BigramTokenizer
{
  grn_ctx *ctx;
  grn_obj *db;
  grn_obj *table;
} BigramTokenizer;

static void bigram_tokenizer_delete(Fts5Tokenizer *pTokenizer)
{
  BigramTokenizer *p = (BigramTokenizer *)pTokenizer;
  if (p)
  {
    if (p->table)
    {
      grn_obj_close(p->ctx, p->table);
    }
    if (p->db)
    {
      grn_obj_close(p->ctx, p->db);
    }
    if (p->ctx)
    {
      grn_ctx_close(p->ctx);
    }
    sqlite3_free(p);
  }
}

static int bigram_tokenizer_create(void *pUserData, const char **azArg, int nArg, Fts5Tokenizer **ppOut)
{
  BigramTokenizer *pTokenizer;

  (void)pUserData;
  (void)azArg;
  (void)nArg;

  if (!groonga_initialized)
  {
    grn_init();
    groonga_initialized = 1;
  }

  pTokenizer = (BigramTokenizer *)sqlite3_malloc(sizeof(BigramTokenizer));
  if (!pTokenizer)
  {
    return SQLITE_NOMEM;
  }
  memset(pTokenizer, 0, sizeof(BigramTokenizer));

  pTokenizer->ctx = grn_ctx_open(0);
  if (!pTokenizer->ctx)
  {
    bigram_tokenizer_delete((Fts5Tokenizer *)pTokenizer);
    return SQLITE_ERROR;
  }

  pTokenizer->db = grn_db_create(pTokenizer->ctx, NULL, NULL);
  if (!pTokenizer->db)
  {
    bigram_tokenizer_delete((Fts5Tokenizer *)pTokenizer);
    return SQLITE_ERROR;
  }

  grn_obj_flags table_flags = GRN_OBJ_TABLE_PAT_KEY;
  grn_obj *key_type = grn_ctx_at(pTokenizer->ctx, GRN_DB_SHORT_TEXT);
  pTokenizer->table = grn_table_create(pTokenizer->ctx, "lexicon", 7, NULL, table_flags, key_type, NULL);
  if (!pTokenizer->table)
  {
    bigram_tokenizer_delete((Fts5Tokenizer *)pTokenizer);
    return SQLITE_ERROR;
  }

  grn_obj tokenizer_str;
  GRN_TEXT_INIT(&tokenizer_str, 0);
  GRN_TEXT_SET(pTokenizer->ctx, &tokenizer_str,
               "TokenNgram(\"unit\", 2, \"report_source_location\", true)",
               strlen("TokenNgram(\"unit\", 2, \"report_source_location\", true)"));
  grn_obj_set_info(pTokenizer->ctx, pTokenizer->table, GRN_INFO_DEFAULT_TOKENIZER, &tokenizer_str);
  GRN_OBJ_FIN(pTokenizer->ctx, &tokenizer_str);

  grn_obj normalizer_str;
  GRN_TEXT_INIT(&normalizer_str, 0);
  GRN_TEXT_SET(pTokenizer->ctx, &normalizer_str,
               "NormalizerAuto(\"report_source_offset\", true)",
               strlen("NormalizerAuto(\"report_source_offset\", true)"));
  grn_obj_set_info(pTokenizer->ctx, pTokenizer->table, GRN_INFO_NORMALIZERS, &normalizer_str);
  GRN_OBJ_FIN(pTokenizer->ctx, &normalizer_str);

  *ppOut = (Fts5Tokenizer *)pTokenizer;
  return SQLITE_OK;
}

static int bigram_tokenizer_tokenize_impl(
    BigramTokenizer *p,
    void *pCtx,
    int flags,
    const char *pText, int nText,
    int (*xToken)(
        void *pCtx,
        int tflags,
        const char *pToken,
        int nToken,
        int iStart,
        int iEnd))
{

  grn_tokenize_mode mode = GRN_TOKENIZE_ONLY;

  grn_token_cursor *cursor = grn_token_cursor_open(
      p->ctx, p->table, pText, nText, mode, 0);

  if (!cursor)
  {
    return SQLITE_ERROR;
  }

  while (grn_token_cursor_get_status(p->ctx, cursor) == GRN_TOKEN_CURSOR_DOING)
  {
    grn_token_cursor_next(p->ctx, cursor);

    grn_token *token = grn_token_cursor_get_token(p->ctx, cursor);
    if (token)
    {
      size_t token_len = 0;
      const char *token_str = grn_token_get_data_raw(p->ctx, token, &token_len);

      if (token_str && token_len > 0)
      {
        grn_token_status status = grn_token_get_status(p->ctx, token);

        uint64_t start_offset = grn_token_get_source_offset(p->ctx, token);
        uint32_t source_length = grn_token_get_source_length(p->ctx, token);
        uint64_t end_offset = start_offset + source_length;

        if ((flags & FTS5_TOKENIZE_QUERY) &&
            (flags & FTS5_TOKENIZE_DOCUMENT) == 0)
        {
          if ((status & GRN_TOKEN_UNMATURED) && (status & GRN_TOKEN_OVERLAP))
          {
            continue;
          }
        }

        char *token_copy = malloc(token_len + 1);
        if (!token_copy)
        {
          grn_token_cursor_close(p->ctx, cursor);
          return SQLITE_NOMEM;
        }
        memcpy(token_copy, token_str, token_len);
        token_copy[token_len] = '\0';

        int rc = xToken(pCtx, 0, token_copy, (int)token_len, (int)start_offset, (int)end_offset);
        free(token_copy);
        if (rc != SQLITE_OK)
        {
          grn_token_cursor_close(p->ctx, cursor);
          return rc;
        }
      }
    }
  }

  grn_token_cursor_close(p->ctx, cursor);
  return SQLITE_OK;
}

static int bigram_tokenizer_tokenize(Fts5Tokenizer *pTokenizer,
                                     void *pCtx,
                                     int flags,
                                     const char *pText, int nText,
                                     const char *pLocale, int nLocale,
                                     int (*xToken)(
                                         void *pCtx,
                                         int tflags,
                                         const char *pToken,
                                         int nToken,
                                         int iStart,
                                         int iEnd))
{

  BigramTokenizer *p = (BigramTokenizer *)pTokenizer;

  (void)pLocale;
  (void)nLocale;

  if (!p || !pText || !xToken)
  {
    return SQLITE_ERROR;
  }

  if (nText < 0)
  {
    nText = strlen(pText);
  }

  if (nText == 0)
  {
    return SQLITE_OK;
  }

  return bigram_tokenizer_tokenize_impl(p, pCtx, flags, pText, nText, xToken);
}

static fts5_tokenizer_v2 bigram_tokenizer = {
    2,
    bigram_tokenizer_create,
    bigram_tokenizer_delete,
    bigram_tokenizer_tokenize};

/*
** Return a pointer to the fts5_api pointer for database connection db.
** If an error occurs, return NULL and leave an error in the database
** handle (accessible using sqlite3_errcode()/errmsg()).
*/
static fts5_api *fts5_api_from_db(sqlite3 *db)
{
  fts5_api *pRet = 0;
  sqlite3_stmt *pStmt = 0;

  if (SQLITE_OK == sqlite3_prepare(db, "SELECT fts5(?1)", -1, &pStmt, 0))
  {
    sqlite3_bind_pointer(pStmt, 1, (void *)&pRet, "fts5_api_ptr", NULL);
    sqlite3_step(pStmt);
  }
  sqlite3_finalize(pStmt);
  return pRet;
}

#ifdef _WIN32
__declspec(dllexport)
#endif
int sqlite3_bigram_init(
    sqlite3 *db,
    char **pzErrMsg,
    const sqlite3_api_routines *pApi)
{
  int rc = SQLITE_OK;
  SQLITE_EXTENSION_INIT2(pApi);

  fts5_api *pFts5Api = fts5_api_from_db(db);
  if (!pFts5Api)
  {
    *pzErrMsg = sqlite3_mprintf("Failed to get FTS5 API");
    return SQLITE_ERROR;
  }

  if (!pFts5Api->xCreateTokenizer_v2)
  {
    *pzErrMsg = sqlite3_mprintf("xCreateTokenizer_v2 not available");
    return SQLITE_ERROR;
  }

  rc = pFts5Api->xCreateTokenizer_v2(pFts5Api, "bigram", 0, &bigram_tokenizer, 0);
  if (rc != SQLITE_OK)
  {
    *pzErrMsg = sqlite3_mprintf("Failed to create tokenizer: %d", rc);
  }

  return rc;
}
