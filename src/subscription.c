/* @@@LICENSE
*
*      Copyright (c) 2008-2012 Hewlett-Packard Development Company, L.P.
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
* http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*
* LICENSE@@@ */


#include <glib.h>
#include <errno.h>
#include <string.h>
#include <pthread.h>

#include <luna-service2/lunaservice.h>
#include "message.h"
#include "base.h"
#include "subscription.h"

/**
 * @addtogroup LunaServiceInternals
 * @{
 */

/** 
* @brief Internal representation of a subscription list.
*/
typedef GPtrArray _SubList;

/** 
* @brief One subscription.
*/
typedef struct _Subscription
{
    LSMessage       *message;
    GPtrArray       *keys;

    LSMessageToken   serverStatusWatch;

    int              ref;

} _Subscription;

/** 
* @brief Internal struct that contains all the subscriptions.
*/
struct _Catalog {

    pthread_mutex_t  lock;

    LSHandle  *sh;

    // each key is user defined 
    // each token is ':sender.connection.serial'
    
    GHashTable *token_map;           //< map of token -> _Subscription
    GHashTable *subscription_lists;  //< map from key ->
                                     //   list of tokens (_SubList)

    LSFilterFunc cancel_function;
    void*        cancel_function_ctx;
};

/** 
* @brief User reference to a subscription list.
*/
struct LSSubscriptionIter {

    _SubList *tokens;          //< copy of the subscription list
    _Catalog *catalog;

    GSList   *seen_messages;   //< ref-counted references to messages iterated
    int index;
};

static bool _subscriber_down(LSHandle *sh, LSMessage *message, void *ctx);
static void _SubscriptionRelease(_Catalog *catalog, _Subscription *subs);

static void
_CatalogLock(_Catalog *catalog)
{
    pthread_mutex_lock(&catalog->lock);
}

static void
_CatalogUnlock(_Catalog *catalog)
{
    pthread_mutex_unlock(&catalog->lock);
}

static void
_SubscriptionRemove(_Catalog *catalog, _Subscription *subs, const char *token)
{
    _CatalogLock(catalog);
    g_hash_table_remove(catalog->token_map, token);
    _CatalogUnlock(catalog);

    _SubscriptionRelease(catalog, subs);
}

static void
_SubscriptionFree(_Catalog *catalog, _Subscription *subs)
{
    if (subs)
    {
        LSMessageUnref(subs->message);

        if (subs->keys)
        {
            g_ptr_array_foreach(subs->keys, (GFunc)g_free, NULL);
            g_ptr_array_free(subs->keys, TRUE);
        }

        if (subs->serverStatusWatch)
        {
            bool retVal;
            LSError lserror;
            LSErrorInit(&lserror);
            retVal = LSCallCancel(catalog->sh, subs->serverStatusWatch,
                        &lserror);
            if (!retVal)
            {
                g_critical("%s Could not cancel server status watch",
                    __FUNCTION__);
                LSErrorPrint(&lserror, stderr);
                LSErrorFree(&lserror);
            }
        }

#ifdef MEMCHECK
        memset(subs, 0xFF, sizeof(_Subscription));
#endif

        g_free(subs);
    }
}

static _Subscription *
_SubscriptionAcquire(_Catalog *catalog, const char *uniqueToken)
{
    _CatalogLock(catalog);

    _Subscription *subs=
        g_hash_table_lookup(catalog->token_map, uniqueToken);
    if (subs)
    {
        LS_ASSERT(g_atomic_int_get(&subs->ref) > 0);
        g_atomic_int_inc(&subs->ref);
    }

    _CatalogUnlock(catalog);

    return subs;
}

static void
_SubscriptionRelease(_Catalog *catalog, _Subscription *subs)
{
    if (subs)
    {
        LS_ASSERT(g_atomic_int_get(&subs->ref) > 0);

        if (g_atomic_int_dec_and_test(&subs->ref))
        {
            _SubscriptionFree(catalog, subs);
        }
    }
}

/** 
* @brief Create a new subscription.
* 
* @param  message 
* 
* @retval
*/
static _Subscription *
_SubscriptionNew(LSHandle *sh, LSMessage *message)
{
    _Subscription *subs;
    bool retVal;

    subs = g_new0(_Subscription,1);
    if (!subs) goto error;

    subs->ref = 1;

    subs->keys = g_ptr_array_new();
    if (!subs->keys) goto error;

    LSMessageRef(message);
    subs->message = message;

    char *payload = g_strdup_printf("{\"serviceName\":\"%s\"}",
            LSMessageGetSender(message));
    if (!payload) goto error;

    LSError lserror;
    LSErrorInit(&lserror);

    LSMessageToken token = LSMessageGetToken(message);

    retVal = LSCall(sh, "palm://com.palm.bus/signal/registerServerStatus",
           payload, _subscriber_down, (void*)token,
           &subs->serverStatusWatch, &lserror);
    g_free(payload);

    if (!retVal)
    {
        LSErrorPrint(&lserror, stderr);
        LSErrorFree(&lserror);
        goto error;
    }

    return subs;

error:
    _SubscriptionFree(sh->catalog, subs);
    return NULL;
}

/** 
* @brief Create new subscription List
* 
* @retval
*/
static _SubList *
_SubListNew()
{
    return g_ptr_array_new();
}

static void
_SubListFree(_SubList *tokens)
{
    if (!tokens) return;

    g_ptr_array_foreach(tokens, (GFunc)g_free, NULL);
    g_ptr_array_free(tokens, TRUE);
}

static int
_SubListLen(_SubList *tokens)
{
    if (!tokens) return 0;
    return tokens->len;
}

/** 
* @brief Add _SubList.
* 
* @param  tokens 
* @param  data 
*/
static void
_SubListAdd(_SubList *tokens, char *data)
{
    if (tokens && data)
        g_ptr_array_add(tokens, data);
}

static _SubList* 
_SubListDup(_SubList *src)
{
    _SubList *dst = NULL;

    if (src)
    {
        dst = _SubListNew();

        int i;
        for (i = 0; i < src->len; i++)
        {
            char *tok = g_ptr_array_index(src, i);
            g_ptr_array_add(dst, g_strdup(tok));
        }
    }

    return dst;
}

/** 
* @brief Remove from _SubList.  This is more expensive.
* 
* @param  tokens 
* @param  data 
*/
static void
_SubListRemove(_SubList *tokens, const char *data)
{
    if (!tokens) return;

    int i;
    for (i = 0; i < tokens->len; i++)
    {
        char *tok = g_ptr_array_index(tokens, i);
        if (strncmp(tok, data, 50) == 0)
        {
            g_ptr_array_remove_index(tokens, i);
            g_free(tok);
            break;
        }
    }
}

static bool
g_char_ptr_array_contains(GPtrArray *array, const char *data)
{
    if (!array) return false;

    int i;
    for (i = 0; i < array->len; i++)
    {
        char *tok = g_ptr_array_index(array, i);
        if (strcmp(tok, data) == 0)
        {
            return true;
        }
    }
    return false;
}

static bool
_SubListContains(_SubList *tokens, const char *data)
{
    if (!tokens) return false;

    int i;
    for (i = 0; i < tokens->len; i++)
    {
        char *tok = g_ptr_array_index(tokens, i);
        if (strncmp(tok, data, 50) == 0)
        {
            return true;
        }
    }
    return false;
}

const char *
_SubListGet(_SubList *tokens, int i)
{
    if (i < 0 || i >= tokens->len)
    {
        g_critical("%s: attempting to get out of range subscription %d\n"
               "It is possible you forgot to follow the pattern: "
               " LSSubscriptionHasNext() + LSSubscriptionNext()",
            __FUNCTION__, i);
        return NULL;
    }

    LS_ASSERT(i >= 0 && i < tokens->len);
    return g_ptr_array_index(tokens, i);
}

_Catalog *
_CatalogNew(LSHandle *sh)
{
    _Catalog *catalog = g_new0(_Catalog, 1);
    if (!catalog) goto error_before_mutex;

    pthread_mutex_init(&catalog->lock, NULL);

    catalog->token_map = g_hash_table_new_full(
            g_str_hash, g_str_equal, g_free, NULL);
    if (!catalog->token_map) goto error;

    catalog->subscription_lists = g_hash_table_new_full(
            g_str_hash, g_str_equal, g_free, (GDestroyNotify)_SubListFree);
    if (!catalog->subscription_lists) goto error;

    catalog->sh = sh;

    return catalog;

error:
    pthread_mutex_destroy(&catalog->lock);

error_before_mutex:

    _CatalogFree(catalog);
    return NULL;
}

gboolean _TokenMapFree(gpointer key, gpointer value, gpointer user_data)
{
    _Subscription *subs = (_Subscription *) value;
    _Catalog *catalog = (_Catalog *) user_data;
    _SubscriptionRelease(catalog, subs);
    return true;
}

void
_CatalogFree(_Catalog *catalog)
{
    if (catalog)
    {
        if (catalog->token_map)
        {
            g_hash_table_foreach_remove(catalog->token_map, _TokenMapFree, catalog);
            g_hash_table_destroy(catalog->token_map);
        }
        if (catalog->subscription_lists)
        {
            g_hash_table_destroy(catalog->subscription_lists);
        }

#ifdef MEMCHECK
        memset(catalog, 0xFF, sizeof(_Catalog));
#endif

        g_free(catalog);
    }
}

static bool
_CatalogAdd(_Catalog *catalog, const char *key,
              LSMessage *message, LSError *lserror)
{
    bool retVal = false;
    const char *token = LSMessageGetUniqueToken(message);
    if (!token)
    {
        _LSErrorSet(lserror, -ENOMEM, "Out of memory");
        return false;
    }

    _CatalogLock(catalog);

    _SubList *list =
        g_hash_table_lookup(catalog->subscription_lists, key);
    if (!list)
    {
        list = _SubListNew();
        g_hash_table_replace(catalog->subscription_lists,
                             g_strdup(key), list);
    }

    if (!list)
    {
        _LSErrorSet(lserror, -ENOMEM, "Out of memory");
        goto cleanup;
    }

    _Subscription *subs = g_hash_table_lookup(catalog->token_map, token);
    if (!subs)
    {
        subs = _SubscriptionNew(catalog->sh, message);
        if (subs)
        {
            g_hash_table_replace(catalog->token_map, g_strdup(token), subs);
        }
        else
        {
            goto cleanup;
        }
    }
    LS_ASSERT(subs->message == message);

    if (!_SubListContains(list, token))
    {
        _SubListAdd(list, g_strdup(token));
    }

    if (!g_char_ptr_array_contains(subs->keys, key))
    {
        g_ptr_array_add(subs->keys, g_strdup(key));
    }

    retVal = true;

cleanup:
    _CatalogUnlock(catalog);
    return retVal;
}

static bool
_CatalogRemoveToken(_Catalog *catalog, const char *token,
                             bool notify)
{
    _Subscription *subs = _SubscriptionAcquire(catalog, token);
    if (!subs) return false; 

    if (notify && catalog->cancel_function)
    {
        catalog->cancel_function(catalog->sh,
                subs->message, catalog->cancel_function_ctx);
    }

    _CatalogLock(catalog);
    int i;
    for (i = 0; i < subs->keys->len; i++)
    {
        const char *key = g_ptr_array_index(subs->keys, i);

        _SubList *sub_list =
            g_hash_table_lookup(catalog->subscription_lists, key);

        _SubListRemove(sub_list, token);

        if (_SubListLen(sub_list) == 0)
        {
            g_hash_table_remove(catalog->subscription_lists, key);
        }
    }
    _CatalogUnlock(catalog);

    _SubscriptionRemove(catalog, subs, token);

    _SubscriptionRelease(catalog, subs);

    return true;
}

bool
_CatalogHandleCancel(_Catalog *catalog, LSMessage *cancelMsg,
                     LSError *lserror)
{
    const char *sender;
    int token;
    struct json_object *tokenObj = NULL;

    const char *payload = LSMessageGetPayload(cancelMsg);

    struct json_object *object = json_tokener_parse(payload);
    if (JSON_ERROR(object))
    {
        _LSErrorSet(lserror, -EINVAL, "Invalid json");
        goto error;
    }

    sender = LSMessageGetSender(cancelMsg);

    if (!json_object_object_get_ex(object, "token", &tokenObj))
    {
        _LSErrorSet(lserror, -EINVAL, "Invalid json");
        goto error;
    }

    token = json_object_get_int(tokenObj);

    char *uniqueToken = g_strdup_printf("%s.%d", sender, token);
    if (!uniqueToken)
    {
        _LSErrorSet(lserror, -ENOMEM, "Out of memory");
        goto error;
    }

    _CatalogRemoveToken(catalog, uniqueToken, true);

    g_free(uniqueToken);
    if (!JSON_ERROR(object)) json_object_put(object);
    return true;

error:
    if (!JSON_ERROR(object)) json_object_put(object);
    return false;
}

static _SubList*
_CatalogGetSubList_unlocked(_Catalog *catalog, const char *key)
{
    _SubList *tokens =
        g_hash_table_lookup(catalog->subscription_lists, key);

    return tokens;
}

static bool
_subscriber_down(LSHandle *sh, LSMessage *message, void *ctx)
{
    bool connected;
    const char *serviceName;

    struct json_object *connectedObj = NULL;
    struct json_object *serviceNameObj = NULL;

    LSMessageToken token = (LSMessageToken)ctx;

    const char *payload = LSMessageGetPayload(message);
    struct json_object *object = json_tokener_parse(payload);

    if (JSON_ERROR(object))
    {
        g_critical("%s: Invalid JSON: %s", __func__, payload);
        goto error;
    }

    if (!json_object_object_get_ex(object, "connected", &connectedObj)) goto error;
    if (!json_object_object_get_ex(object, "serviceName", &serviceNameObj)) goto error;

    connected = json_object_get_boolean(connectedObj);
    serviceName = json_object_get_string(serviceNameObj);

    if (!connected)
    {
        char *uniqueToken = g_strdup_printf("%s.%ld", serviceName, token);

        if (uniqueToken)
        {
            _Subscription *subs = _SubscriptionAcquire(sh->catalog, uniqueToken);
            if (subs)
            {
                _CatalogRemoveToken(sh->catalog, uniqueToken, true);
                _SubscriptionRelease(sh->catalog, subs);
            }
        }

        g_free(uniqueToken);
    }

error:
    if (!JSON_ERROR(object)) json_object_put(object);
    return true;
}

bool
_LSSubscriptionGetJson(LSHandle *sh, struct json_object **ret_obj, LSError *lserror)
{
    _Catalog *catalog = sh->catalog;
    const char *key = NULL;
    _SubList *sub_list = NULL;
    GHashTableIter iter;

    struct json_object *true_obj = NULL;
    struct json_object *array = NULL;
    struct json_object *cur_obj = NULL;
    struct json_object *sub_array = NULL;
    struct json_object *key_name = NULL;
    struct json_object *message_obj = NULL;
    struct json_object *sub_array_item = NULL;
    struct json_object *unique_name_obj = NULL;
    struct json_object *service_name_obj = NULL;
    
    *ret_obj = json_object_new_object();
    if (JSON_ERROR(ret_obj)) goto error;
       
    true_obj = json_object_new_boolean(true);
    if (JSON_ERROR(true_obj)) goto error;
 
    array = json_object_new_array();
    if (JSON_ERROR(array)) goto error;

    /* returnValue: true,
     * subscriptions: [
     *  { key: key_name, subscribers: [{unique_name: , service_name: }, ...] },
     *  ...
     * ]
     */
    _CatalogLock(catalog);

    g_hash_table_iter_init(&iter, catalog->subscription_lists);

    while (g_hash_table_iter_next(&iter, (gpointer)&key, (gpointer)&sub_list))
    {
        cur_obj = json_object_new_object();
        if (JSON_ERROR(cur_obj)) goto error;

        sub_array = json_object_new_array();
        if (JSON_ERROR(sub_array)) goto error;
                
        key_name = json_object_new_string(key);
        if (JSON_ERROR(key_name)) goto error;

        /* iterate over SubList */
        int i = 0;
        const char *token = NULL;
        const int len = _SubListLen(sub_list);
        for (i = 0; i < len; i++)
        {
            token = _SubListGet(sub_list, i);

            if (token)
            {
                _Subscription *sub = g_hash_table_lookup(catalog->token_map, token);

                if (!sub) continue;
    
                LSMessage *msg = sub->message;
                const char *unique_name = LSMessageGetSender(msg);
                const char *service_name = LSMessageGetSenderServiceName(msg);
                const char *message_body = LSMessageGetPayload(msg);
                
                /* create subscribers item and add to sub_array */
                sub_array_item = json_object_new_object();
                if (JSON_ERROR(sub_array_item)) goto error;

                unique_name_obj = unique_name ? json_object_new_string(unique_name)
                                              : json_object_new_string("");
                if (JSON_ERROR(unique_name_obj)) goto error;

                service_name_obj = service_name ? json_object_new_string(service_name)
                                                : json_object_new_string("");
                if (JSON_ERROR(service_name_obj)) goto error;
                
                message_obj = message_body ? json_object_new_string(message_body)
                                                : json_object_new_string("");                                                
                if (JSON_ERROR(message_obj)) goto error;

                json_object_object_add(sub_array_item, "unique_name", unique_name_obj);
                json_object_object_add(sub_array_item, "service_name", service_name_obj);
                json_object_object_add(sub_array_item, "subscription_message", message_obj);
                json_object_array_add(sub_array, sub_array_item);
               
                sub_array_item = NULL;
                unique_name_obj = NULL;
                service_name_obj = NULL;
                message_obj = NULL;
            }
        }
        json_object_object_add(cur_obj, "key", key_name);
        json_object_object_add(cur_obj, "subscribers", sub_array);
        json_object_array_add(array, cur_obj);
        key_name = NULL; 
        cur_obj = NULL;
        sub_array = NULL;
    }
    
    json_object_object_add(*ret_obj, "returnValue", true_obj);
    json_object_object_add(*ret_obj, "subscriptions", array);

    _CatalogUnlock(catalog);

    return true;

error:
    _CatalogUnlock(catalog);
    
    if (!JSON_ERROR(*ret_obj)) json_object_put(*ret_obj);
    if (!JSON_ERROR(true_obj)) json_object_put(true_obj);
    if (!JSON_ERROR(array)) json_object_put(array);
    
    if (!JSON_ERROR(cur_obj)) json_object_put(cur_obj);
    if (!JSON_ERROR(sub_array)) json_object_put(sub_array);
    if (!JSON_ERROR(key_name)) json_object_put(key_name);
 
    if (!JSON_ERROR(sub_array_item)) json_object_put(sub_array_item); 
    if (!JSON_ERROR(unique_name_obj)) json_object_put(unique_name_obj); 
    if (!JSON_ERROR(service_name_obj)) json_object_put(service_name_obj); 
    
    return false;
}

/* @} END OF LunaServiceInternals */

/**
 * @addtogroup LunaServiceSubscription
 *
 * @{
 */

/** 
* @brief Register a callback to be called when subscription cancelled.
*
*  Callback may be called when client cancels subscription via LSCallCancel()
*  or if the client drops off the bus.
* 
* @param  sh 
* @param  cancelFunction 
* @param  ctx 
* @param  lserror 
* 
* @retval
*/
bool
LSSubscriptionSetCancelFunction(LSHandle *sh, LSFilterFunc cancelFunction,
                                void *ctx, LSError *lserror)
{
    LSHANDLE_VALIDATE(sh);

    sh->catalog->cancel_function = cancelFunction;
    sh->catalog->cancel_function_ctx = ctx;
    return true;
}

/** 
* @brief Add a subscription to a list associated with 'key'.
* 
* @param  sh 
* @param  key 
* @param  message 
* @param  lserror 
* 
* @retval
*/
bool
LSSubscriptionAdd(LSHandle *sh, const char *key,
                  LSMessage *message, LSError *lserror)
{
    LSHANDLE_VALIDATE(sh);

    return _CatalogAdd(sh->catalog, key, message, lserror);
}

/** 
* @brief Acquire an iterator to iterate through the subscription
*        for 'key'.
* 
* @param  sh 
* @param  key 
* @param  *ret_iter 
* @param  lserror 
* 
* @retval
*/
bool
LSSubscriptionAcquire(LSHandle *sh, const char *key,
                  LSSubscriptionIter **ret_iter, LSError *lserror)
{
    LSHANDLE_VALIDATE(sh);

    _Catalog *catalog = sh->catalog;
    LSSubscriptionIter *iter = g_new0(LSSubscriptionIter, 1);
    if (!iter)
    {
        _LSErrorSet(lserror, -ENOMEM, "Out of memory");
        return false;
    }

    _CatalogLock(catalog);
    _SubList *tokens = _CatalogGetSubList_unlocked(catalog, key);
    iter->tokens = _SubListDup(tokens);
    _CatalogUnlock(catalog);

    iter->catalog = catalog;
    iter->index = -1;
    iter->seen_messages = NULL;

    if (ret_iter)
    {
        *ret_iter = iter;
    }

    return true;
}

/** 
* @brief Frees up resources for LSSubscriptionIter.
* 
* @param  iter 
*/
void
LSSubscriptionRelease(LSSubscriptionIter *iter)
{
    GSList *seen_iter = iter->seen_messages;
    while (seen_iter)
    {
        LSMessage *msg = (LSMessage*)seen_iter->data;
        LSMessageUnref(msg);

        seen_iter = seen_iter->next;
    }

    _SubListFree(iter->tokens);
    g_slist_free(iter->seen_messages);
    g_free(iter);
}

/** 
* @brief Returns whether there is a next item in subscription.
* 
* @param  iter 
* 
* @retval
*/
bool
LSSubscriptionHasNext(LSSubscriptionIter *iter)
{
    if (!iter->tokens)
    {
        return false;
    }

    return iter->index+1 < _SubListLen(iter->tokens);
}

/** 
* @brief Obtain the next subscription message.
* 
* @param  iter 
* 
* @retval
*/
LSMessage *
LSSubscriptionNext(LSSubscriptionIter *iter)
{
    _Subscription *subs = NULL;
    LSMessage *message = NULL;

    iter->index++;
    const char *tok = _SubListGet(iter->tokens, iter->index);
    if (tok)
    {
        subs = _SubscriptionAcquire(iter->catalog, tok);
        if (subs)
        {
            message = subs->message;
            LSMessageRef(message);

            iter->seen_messages =
                g_slist_prepend(iter->seen_messages, message);

            _SubscriptionRelease(iter->catalog, subs);
        }
    }

    return message;
}

/** 
* @brief Remove the last subscription returned by LSSubscriptionNext().
* 
* @param  iter 
*/
void
LSSubscriptionRemove(LSSubscriptionIter *iter)
{
    const char *tok = _SubListGet(iter->tokens, iter->index);
    if (tok)
    {
        _CatalogRemoveToken(iter->catalog, tok, false);
    }
}

/** 
* @brief Sends a message to subscription list with name 'key'.
* 
* @param  sh 
* @param  key 
* @param  payload 
* @param  lserror 
* 
* @retval
*/
bool
LSSubscriptionReply(LSHandle *sh, const char *key,
                    const char *payload, LSError *lserror)
{
    LSHANDLE_VALIDATE(sh);

    bool retVal = true;
    _Catalog *catalog = sh->catalog;

    _CatalogLock(catalog);

    _SubList *tokens = _CatalogGetSubList_unlocked(catalog, key);
    if (!tokens)
    {
        retVal = true;
        goto cleanup;
    }

    int i;
    for (i = 0; i < tokens->len; i++)
    {
        char *tok = g_ptr_array_index(tokens, i);

        _Subscription *subs =
            g_hash_table_lookup(catalog->token_map, tok);
        if (!subs) continue;

        LSMessage *message = subs->message;

        retVal = LSMessageReply(sh, message, payload, lserror);
        if (!retVal) goto cleanup;
    }
cleanup:
    _CatalogUnlock(catalog);
    return retVal;
}

/** 
* @brief Post a notification to all subscribers with name 'key'.
*
* This is equivalent to:
* LSSubscriptionReply(public_bus, ...)
* LSSubscriptionReply(private_bus, ...)
* 
* @param  psh 
* @param  key 
* @param  payload 
* @param  lserror 
* 
* @retval
*/
bool
LSSubscriptionRespond(LSPalmService *psh, const char *key,
                      const char *payload, LSError *lserror)
{
    LSHandle *public_bus = LSPalmServiceGetPublicConnection(psh);
    LSHandle *private_bus = LSPalmServiceGetPrivateConnection(psh);
    bool retVal;

    retVal = LSSubscriptionReply(public_bus, key, payload, lserror);
    if (!retVal) return retVal;

    retVal = LSSubscriptionReply(private_bus, key, payload, lserror);
    if (!retVal) return retVal;

    return true;
}

/** 
* @brief If message contains subscribe:true, add the message
         to subscription list using the default key '/category/method'.
*
*        This is equivalent to LSSubscriptionAdd(sh, key, message, lserror)
*        where the key is LSMessageGetKind(message).
* 
* @param  sh 
* @param  message 
* @param  subscribed 
* @param  lserror 
* 
* @retval
*/
bool
LSSubscriptionProcess (LSHandle *sh, LSMessage *message, bool *subscribed, 
                        LSError *lserror)
{
    bool retVal = false;
    bool subscribePayload = false;
    struct json_object *subObj = NULL;

    const char *payload = LSMessageGetPayload(message);
    struct json_object *object = json_tokener_parse(payload);

    if (JSON_ERROR(object))
    {
        _LSErrorSet(lserror, -1, "Unable to parse JSON: %s", payload);
        goto exit;
    }

    if (!json_object_object_get_ex(object, "subscribe", &subObj))
    {
        subscribePayload = false;
        /* FIXME: I think retVal should be false, but I don't know if anyone
         * is relying on this behavior. If set to false, make sure to set
         * LSError */
        retVal = true;
    }
    else
    {
        subscribePayload = json_object_get_boolean(subObj);
        retVal = true;
    }

    if (subscribePayload)
    {
        const char *key = LSMessageGetKind(message);
        retVal = LSSubscriptionAdd(sh, key, message, lserror);
    }

    if (retVal && subscribePayload)
    {
        *subscribed = true;
    }
    else
    {
        *subscribed = false;
    }

exit:
    if (!JSON_ERROR(object)) json_object_put(object);

    return retVal;
}

/** 
* @brief Posts a message to all in subscription '/category/method'.
*        This is equivalent to:
*        LSSubscriptionReply(sh, '/category/method', payload, lserror)
*
* @deprecated Please use LSSubscriptionReply() instead.
* 
* @param  sh 
* @param  category 
* @param  method 
* @param  payload 
* @param  lserror 
* 
* @retval
*/
bool
LSSubscriptionPost(LSHandle *sh, const char *category,
                   const char *method,
                   const char *payload, LSError *lserror)
{
    LSHANDLE_VALIDATE(sh);

    bool retVal = false;
    char *key = _LSMessageGetKindHelper(category, method);
    if (!key)
    {
        _LSErrorSet(lserror, -ENOMEM, "Out of memory");
        return false;
    }

    retVal = LSSubscriptionReply(sh, key, payload, lserror);

    g_free(key);
    return retVal;
}

/* @} END OF LunaServiceSubscription */
