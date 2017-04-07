/*
 * Description: 
 *     History: yang@haipo.me, 2017/04/06, create
 */

# include "me_config.h"
# include "me_history.h"

static MYSQL *mysql_conn;
static nw_job *job;
static dict_t *dict_sql;
static nw_timer timer;

enum {
    HISTORY_USER_BALANCE,
    HISTORY_USER_ORDER,
    HISTORY_ORDER_DETAIL,
    HISTORY_ORDER_DEAL,
};

struct dict_sql_key {
    uint32_t type;
    uint32_t hash;
};

static uint32_t dict_sql_hash_function(const void *key)
{
    return dict_generic_hash_function(key, sizeof(struct dict_sql_key));
}

static void *dict_sql_key_dup(const void *key)
{
    struct dict_sql_key *obj = malloc(sizeof(struct dict_sql_key));
    memcpy(obj, key, sizeof(struct dict_sql_key));
    return obj;
}

static int dict_sql_key_compare(const void *key1, const void *key2)
{
    return memcmp(key1, key2, sizeof(struct dict_sql_key));
}

static void dict_sql_key_free(void *key)
{
    free(key);
}

static void dict_sql_val_free(void *val)
{
    sdsfree(val);
}

static void *on_job_init(void)
{
    return mysql_connect(&settings.db_log);
}

static void on_job(nw_job_entry *entry, void *privdata)
{
    MYSQL *conn = privdata;
    sds sql = entry->request;
    log_trace("exec sql: %s", sql);
    while (true) {
        int ret = mysql_real_query(conn, sql, sdslen(sql));
        if (ret != 0) {
            log_error("exec sql: %s fail: %d %s", sql, mysql_errno(conn), mysql_error(conn));
            usleep(1000 * 1000);
            continue;
        }
        break;
    }
}

static void on_job_cleanup(nw_job_entry *entry)
{
    sdsfree(entry->request);
}

static void on_job_release(void *privdata)
{
    mysql_close(privdata);
}

static void on_timer(nw_timer *t, void *privdata)
{
    size_t count = 0;
    dict_iterator *iter = dict_get_iterator(dict_sql);
    dict_entry *entry;
    while ((entry = dict_next(iter)) != NULL) {
        sds sql = entry->val;
        nw_job_add(job, 0, sdsnewlen(sql, sdslen(sql)));
        dict_delete(dict_sql, entry->key);
        count++;
    }

    if (count) {
        log_debug("flush history count: %zu", count);
    }
}

int init_history(void)
{
    mysql_conn = mysql_init(NULL);
    if (mysql_conn == NULL)
        return -__LINE__;
    if (mysql_options(mysql_conn, MYSQL_SET_CHARSET_NAME, settings.db_log.charset) != 0)
        return -__LINE__;

    dict_types dt;
    memset(&dt, 0, sizeof(dt));
    dt.hash_function  = dict_sql_hash_function;
    dt.key_compare    = dict_sql_key_compare;
    dt.key_dup        = dict_sql_key_dup;
    dt.key_destructor = dict_sql_key_free;
    dt.val_destructor = dict_sql_val_free;

    dict_sql = dict_create(&dt, 1024);
    if (dict_sql == 0) {
        return -__LINE__;
    }

    nw_job_type jt;
    memset(&jt, 0, sizeof(jt));
    jt.on_init    = on_job_init;
    jt.on_job     = on_job;
    jt.on_cleanup = on_job_cleanup;
    jt.on_release = on_job_release;

    job = nw_job_create(&jt, 20);
    if (job == NULL)
        return -__LINE__;

    nw_timer_set(&timer, 0.1, true, on_timer, NULL);
    nw_timer_start(&timer);

    return 0;
}

static sds sql_append_mpd(sds sql, mpd_t *val, bool comma)
{
    char *str = mpd_to_sci(val, 0);
    sql = sdscatprintf(sql, "\"%s\"", str);
    if (comma) {
        sql = sdscatprintf(sql, ", ");
    }
    free(str);
    return sql;
}

static sds get_sql(struct dict_sql_key *key)
{
    dict_entry *entry = dict_find(dict_sql, key);
    if (!entry) {
        sds val = sdsempty();
        entry = dict_add(dict_sql, &key, val);
        if (entry == NULL) {
            sdsfree(val);
            return NULL;
        }
    }
    return entry->val;
}

static int append_user_order(order_t *order)
{
    struct dict_sql_key key;
    key.hash = order->user_id % HISTORY_HASH_NUM;
    key.type = HISTORY_USER_ORDER;
    sds sql = get_sql(&key);
    if (sql == NULL)
        return -__LINE__;

    if (sdslen(sql) == 0) {
        sql = sdscatprintf(sql, "INSERT INTO `order_history_%u` (`id`, `create_time`, `finish_time`, `user_id`, "
                "`market`, `t`, `side`, `price`, `amount`, `fee`, `deal_stock`, `deal_money`, `deal_fee`) VALUES ", key.hash);
    } else {
        sql = sdscatprintf(sql, ", ");
    }

    sql = sdscatprintf(sql, "(%"PRIu64", %f, %f, %u, \"%s\", %u, %u, ", order->id,
        order->create_time, order->update_time, order->user_id, order->market, order->type, order->side);
    sql = sql_append_mpd(sql, order->price, true);
    sql = sql_append_mpd(sql, order->amount, true);
    sql = sql_append_mpd(sql, order->fee, true);
    sql = sql_append_mpd(sql, order->deal_stock, true);
    sql = sql_append_mpd(sql, order->deal_money, true);
    sql = sql_append_mpd(sql, order->deal_fee, false);
    sql = sdscatprintf(sql, ")");

    return 0;
}

static int append_order_detail(order_t *order)
{
    struct dict_sql_key key;
    key.hash = order->id % HISTORY_HASH_NUM;
    key.type = HISTORY_ORDER_DETAIL;
    sds sql = get_sql(&key);
    if (sql == NULL)
        return -__LINE__;

    if (sdslen(sql) == 0) {
        sql = sdscatprintf(sql, "INSERT INTO `order_detail_%u` (`id`, `create_time`, `finish_time`, `user_id`, "
                "`market`, `t`, `side`, `price`, `amount`, `fee`, `deal_stock`, `deal_money`, `deal_fee`) VALUES ", key.hash);
    } else {
        sql = sdscatprintf(sql, ", ");
    }

    sql = sdscatprintf(sql, "(%"PRIu64", %f, %f, %u, \"%s\", %u, %u, ", order->id,
        order->create_time, order->update_time, order->user_id, order->market, order->type, order->side);
    sql = sql_append_mpd(sql, order->price, true);
    sql = sql_append_mpd(sql, order->amount, true);
    sql = sql_append_mpd(sql, order->fee, true);
    sql = sql_append_mpd(sql, order->deal_stock, true);
    sql = sql_append_mpd(sql, order->deal_money, true);
    sql = sql_append_mpd(sql, order->deal_fee, false);
    sql = sdscatprintf(sql, ")");

    return 0;
}

static int append_order_deal(double t, uint64_t order_id, uint64_t deal_order_id, mpd_t *amount, mpd_t *price, mpd_t *deal, mpd_t *fee)
{
    struct dict_sql_key key;
    key.hash = order_id % HISTORY_HASH_NUM;
    key.type = HISTORY_ORDER_DEAL;
    sds sql = get_sql(&key);
    if (sql == NULL)
        return -__LINE__;

    if (sdslen(sql) == 0) {
        sql = sdscatprintf(sql, "INSERT INTO `deal_history_%u` (`id`, `time`, `order_id`, `deal_order_id`, `amount`, `price`, `deal`, `fee`) VALUES ", key.hash);
    } else {
        sql = sdscatprintf(sql, ", ");
    }

    sql = sdscatprintf(sql, "(NULL, %f, %"PRIu64", %"PRIu64", ", t, order_id, deal_order_id);
    sql = sql_append_mpd(sql, amount, true);
    sql = sql_append_mpd(sql, price, true);
    sql = sql_append_mpd(sql, deal, true);
    sql = sql_append_mpd(sql, fee, true);
    sql = sdscatprintf(sql, ")");

    return 0;
}

static int append_user_balance(double t, uint32_t user_id, const char *asset, const char *business, mpd_t *change, mpd_t *balance, const char *detail)
{
    struct dict_sql_key key;
    key.hash = user_id % HISTORY_HASH_NUM;
    key.type = HISTORY_USER_BALANCE;
    sds sql = get_sql(&key);
    if (sql == NULL)
        return -__LINE__;

    if (sdslen(sql) == 0) {
        sql = sdscatprintf(sql, "INSERT INTO `balance_history_%u` (`id`, `time`, `user_id`, `asset`, `business`, `change`, `balance`, `detail`) VALUES ", key.hash);
    } else {
        sql = sdscatprintf(sql, ", ");
    }

    char buf[10 * 1024];
    sql = sdscatprintf(sql, "(NULL, %f, %u, \"%s\", \"%s\", ", t, user_id, asset, business);
    sql = sql_append_mpd(sql, change, true);
    sql = sql_append_mpd(sql, balance, true);
    mysql_real_escape_string(mysql_conn, buf, detail, strlen(detail));
    sql = sdscatprintf(sql, "\"%s\")", buf);

    return 0;
}

int append_order_history(order_t *order)
{
    append_user_order(order);
    append_order_detail(order);
    return 0;
}

int append_order_deal_history(double t, uint64_t ask, uint64_t bid, mpd_t *amount, mpd_t *price, mpd_t *deal, mpd_t *ask_fee, mpd_t *bid_fee)
{
    append_order_deal(t, ask, bid, amount, price, deal, ask_fee);
    append_order_deal(t, bid, ask, amount, price, deal, bid_fee);
    return 0;
}

int append_user_balance_history(double t, uint32_t user_id, const char *asset, const char *business, mpd_t *change, mpd_t *balance, const char *detail)
{
    append_user_balance(t, user_id, asset, business, change, balance, detail);
    return 0;
}
