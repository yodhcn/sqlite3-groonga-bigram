#include <groonga.h>
#include <stdlib.h>
#include <inttypes.h>
#include <sqlite3ext.h>
#include <pthread.h>

SQLITE_EXTENSION_INIT1

/* 使用 pthread_once 确保 groonga_init() 只被调用一次 */
static pthread_once_t groonga_init_once = PTHREAD_ONCE_INIT;

/* 线程本地存储结构：每个线程独立的 Groonga 上下文 */
typedef struct ThreadLocalContext
{
  grn_ctx *ctx;   /* Groonga 上下文（线程独占） */
  grn_obj *db;    /* Groonga 数据库对象 */
  grn_obj *table; /* 分词用的词表 */
} ThreadLocalContext;

/* FTS5 Tokenizer 实例结构 */
typedef struct BigramTokenizer
{
  pthread_key_t tls_key; /* 线程本地存储 key */
} BigramTokenizer;

/* 初始化 Groonga 库（封装函数以适配 pthread_once 的签名要求） */
static void init_groonga(void)
{
  grn_init();
}

#define TOKENIZER_STR "TokenNgram(\"n\", 2, \"report_source_location\", true, \"unify_symbol\", false)"
#define NORMALIZER_STR "NormalizerAuto(\"report_source_offset\", true)"

/* 释放线程本地的 Groonga 资源 */
/* 当线程退出时，pthread 会自动调用此函数清理 TLS */
static void free_thread_local_context(void *arg)
{
  ThreadLocalContext *tlc = (ThreadLocalContext *)arg;
  if (!tlc)
    return;

  if (tlc->table)
    grn_obj_close(tlc->ctx, tlc->table);
  if (tlc->db)
    grn_obj_close(tlc->ctx, tlc->db);
  if (tlc->ctx)
    grn_ctx_close(tlc->ctx);
  free(tlc);
}

/* 创建 Groonga db和table，并配置TokenNgram 和NormalizerAuto */
static int create_thread_local_objects(grn_ctx *ctx, grn_obj **db, grn_obj **table)
{
  /* 创建 db */
  *db = grn_db_create(ctx, NULL, NULL);
  if (!*db)
    return SQLITE_ERROR;

  /* 创建 table */
  grn_obj *key_type = grn_ctx_at(ctx, GRN_DB_SHORT_TEXT);
  *table = grn_table_create(ctx, "lexicon", 7, NULL, GRN_OBJ_TABLE_PAT_KEY, key_type, NULL);
  if (!*table)
    return SQLITE_ERROR;

  /* 配置 TokenNgram */
  grn_obj tokenizer_str;
  GRN_TEXT_INIT(&tokenizer_str, 0);
  GRN_TEXT_SET(ctx, &tokenizer_str, TOKENIZER_STR, sizeof(TOKENIZER_STR) - 1);
  grn_obj_set_info(ctx, *table, GRN_INFO_DEFAULT_TOKENIZER, &tokenizer_str);
  GRN_OBJ_FIN(ctx, &tokenizer_str);

  /* 配置 NormalizerAuto */
  grn_obj normalizer_str;
  GRN_TEXT_INIT(&normalizer_str, 0);
  GRN_TEXT_SET(ctx, &normalizer_str, NORMALIZER_STR, sizeof(NORMALIZER_STR) - 1);
  grn_obj_set_info(ctx, *table, GRN_INFO_NORMALIZERS, &normalizer_str);
  GRN_OBJ_FIN(ctx, &normalizer_str);

  return SQLITE_OK;
}

/* 获取当前线程的 Groonga 上下文 */
/* 如果是首次调用，则创建新的上下文并存储到 TLS */
static ThreadLocalContext *get_thread_local_context(BigramTokenizer *p)
{
  /* 尝试从 TLS 获取当前线程的上下文 */
  ThreadLocalContext *tlc = pthread_getspecific(p->tls_key);
  if (tlc)
    return tlc;

  /* 首次调用：为当前线程创建新的上下文 */
  tlc = calloc(1, sizeof(ThreadLocalContext));
  if (!tlc)
    return NULL;

  /* 打开 Groonga 上下文 */
  tlc->ctx = grn_ctx_open(0);
  if (!tlc->ctx || create_thread_local_objects(tlc->ctx, &tlc->db, &tlc->table) != SQLITE_OK)
  {
    free_thread_local_context(tlc);
    return NULL;
  }

  /* 将新创建的上下文存入 TLS */
  if (pthread_setspecific(p->tls_key, tlc) != 0)
  {
    free_thread_local_context(tlc);
    return NULL;
  }

  return tlc;
}

/* 销毁 Tokenizer 实例 */
/* SQLite FTS5 调用此函数删除 tokenizer */
static void bigram_tokenizer_delete(Fts5Tokenizer *pTokenizer)
{
  if (!pTokenizer)
    return;
  BigramTokenizer *p = (BigramTokenizer *)pTokenizer;
  pthread_key_delete(p->tls_key);
  sqlite3_free(p);
}

/* 创建 Tokenizer 实例 */
/* SQLite FTS5 在首次使用 tokenizer 时调用 */
static int bigram_tokenizer_create(void *pUserData, const char **azArg, int nArg, Fts5Tokenizer **ppOut)
{
  (void)pUserData;
  (void)azArg;
  (void)nArg;

  /* 初始化 Groonga（线程安全，只执行一次） */
  pthread_once(&groonga_init_once, init_groonga);

  /* 分配 Tokenizer 实例 */
  BigramTokenizer *p = sqlite3_malloc(sizeof(BigramTokenizer));
  if (!p)
    return SQLITE_NOMEM;

  /* 创建线程本地存储 key，并设置析构函数 */
  /* 析构函数会在线程退出时自动调用，清理该线程的 Groonga 资源 */
  if (pthread_key_create(&p->tls_key, free_thread_local_context) != 0)
  {
    sqlite3_free(p);
    return SQLITE_ERROR;
  }

  *ppOut = (Fts5Tokenizer *)p;
  return SQLITE_OK;
}

/* 发送单个 token 到 FTS5 回调函数 */
static int emit_token(void *pCtx, int tflags, const char *token_str, size_t token_len,
                      uint64_t start_offset, uint32_t source_length,
                      int (*xToken)(void *, int, const char *, int, int, int))
{
  if (token_len == 0)
    return SQLITE_OK;

  /* 复制 token 字符串（因为 xToken 可能会释放传入的指针） */
  char *token_copy = malloc(token_len + 1);
  if (!token_copy)
    return SQLITE_NOMEM;

  memcpy(token_copy, token_str, token_len);
  token_copy[token_len] = '\0';

  /* 调用 FTS5 的回调函数，传递 token 信息 */
  int rc = xToken(pCtx, tflags, token_copy, (int)token_len, (int)start_offset, (int)(start_offset + source_length));
  free(token_copy);
  return rc;
}

/* 核心分词实现函数 */
/* 使用 Groonga 的 token_cursor 遍历文本，提取所有 token */
static int bigram_tokenizer_tokenize_impl(BigramTokenizer *p, void *pCtx, int flags,
                                          const char *pText, int nText,
                                          int (*xToken)(void *, int, const char *, int, int, int))
{
  /* 获取当前线程的 Groonga 上下文 */
  ThreadLocalContext *tlc = get_thread_local_context(p);
  if (!tlc)
    return SQLITE_ERROR;

  /* 打开 Groonga token cursor */
  grn_token_cursor *cursor = grn_token_cursor_open(tlc->ctx, tlc->table, pText, nText, GRN_TOKENIZE_ONLY, 0);
  if (!cursor)
    return SQLITE_ERROR;

  int rc = SQLITE_OK;

  /* 遍历所有 token */
  while (grn_token_cursor_get_status(tlc->ctx, cursor) == GRN_TOKEN_CURSOR_DOING)
  {
    grn_token_cursor_next(tlc->ctx, cursor);

    grn_token *token = grn_token_cursor_get_token(tlc->ctx, cursor);
    if (!token)
      continue;

    /* 获取 token 内容和长度 */
    size_t token_len = 0;
    const char *token_str = grn_token_get_data_raw(tlc->ctx, token, &token_len);
    if (!token_str || token_len == 0)
      continue;

    /* 对于查询模式，跳过未成熟且重叠的 token */
    if ((flags & FTS5_TOKENIZE_QUERY) && !(flags & FTS5_TOKENIZE_DOCUMENT))
    {
      grn_token_status status = grn_token_get_status(tlc->ctx, token);
      if ((status & GRN_TOKEN_UNMATURED) && (status & GRN_TOKEN_OVERLAP))
        continue;
    }

    /* 获取 token 在原文中的位置信息 */
    uint64_t start_offset = grn_token_get_source_offset(tlc->ctx, token);
    uint32_t source_length = grn_token_get_source_length(tlc->ctx, token);

    /* 发送 token 到 FTS5 */
    rc = emit_token(pCtx, 0, token_str, token_len, start_offset, source_length, xToken);
    if (rc != SQLITE_OK)
      break;
  }

  grn_token_cursor_close(tlc->ctx, cursor);
  return rc;
}

/* FTS5 tokenizer 入口函数 */
/* SQLite FTS5 调用此函数进行分词 */
static int bigram_tokenizer_tokenize(Fts5Tokenizer *pTokenizer, void *pCtx, int flags,
                                     const char *pText, int nText,
                                     const char *pLocale, int nLocale,
                                     int (*xToken)(void *, int, const char *, int, int, int))
{
  (void)pLocale;
  (void)nLocale;

  /* 参数校验 */
  if (!pTokenizer || !pText || !xToken)
    return SQLITE_ERROR;
  if (nText < 0)
    nText = strlen(pText);
  if (nText == 0)
    return SQLITE_OK;

  return bigram_tokenizer_tokenize_impl((BigramTokenizer *)pTokenizer, pCtx, flags, pText, nText, xToken);
}

/* FTS5 Tokenizer 接口定义 */
static fts5_tokenizer_v2 bigram_tokenizer = {
    2,                        /* 版本号 */
    bigram_tokenizer_create,  /* 创建函数 */
    bigram_tokenizer_delete,  /* 删除函数 */
    bigram_tokenizer_tokenize /* 分词函数 */
};

/*
** Return a pointer to the fts5_api pointer for database connection db.
** If an error occurs, return NULL and leave an error in the database
** handle (accessible using sqlite3_errcode()/errmsg()).
*/
static fts5_api *fts5_api_from_db(sqlite3 *db)
{
  fts5_api *pRet = NULL;
  sqlite3_stmt *pStmt = NULL;

  if (SQLITE_OK == sqlite3_prepare(db, "SELECT fts5(?1)", -1, &pStmt, 0))
  {
    sqlite3_bind_pointer(pStmt, 1, (void *)&pRet, "fts5_api_ptr", NULL);
    sqlite3_step(pStmt);
  }
  sqlite3_finalize(pStmt);
  return pRet;
}

/* SQLite 扩展入口函数 */
/* 当执行 "SELECT load_extension('./libbigram.so')" 时调用 */
#ifdef _WIN32
__declspec(dllexport)
#endif
int sqlite3_bigram_init(sqlite3 *db, char **pzErrMsg, const sqlite3_api_routines *pApi)
{
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

  int rc = pFts5Api->xCreateTokenizer_v2(pFts5Api, "bigram", 0, &bigram_tokenizer, 0);
  if (rc != SQLITE_OK)
  {
    *pzErrMsg = sqlite3_mprintf("Failed to create tokenizer: %d", rc);
  }
  return rc;
}
