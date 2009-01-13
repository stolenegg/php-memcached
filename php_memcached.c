/*
  +----------------------------------------------------------------------+
  | Copyright (c) 2008 The PHP Group                                     |
  +----------------------------------------------------------------------+
  | This source file is subject to version 3.0 of the PHP license,       |
  | that is bundled with this package in the file LICENSE, and is        |
  | available through the world-wide-web at the following url:           |
  | http://www.php.net/license/3_0.txt.                                  |
  | If you did not receive a copy of the PHP license and are unable to   |
  | obtain it through the world-wide-web, please send a note to          |
  | license@php.net so we can mail you a copy immediately.               |
  +----------------------------------------------------------------------+
  | Authors: Andrei Zmievski <andrei@php.net>                            |
  +----------------------------------------------------------------------+
*/

/* $ Id: $ */

/* TODO
 * - check for key length in setters (maybe)
 * - throw an exception if cas fails due to token mismatch
 * - figure out append/prepend and compressed/serialized data problems
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <php.h>

#ifdef ZTS
#include "TSRM.h"
#endif

#include <php_ini.h>
#include <SAPI.h>
#include <ext/standard/info.h>
#include <zend_extensions.h>
#include <zend_exceptions.h>
#include <ext/standard/php_smart_str.h>
#include <ext/standard/php_var.h>
#include <libmemcached/memcached.h>

#include <zlib.h>

#include "php_memcached.h"

#define MEMC_OPT_COMPRESSION   -1001
#define MEMC_OPT_PREFIX_KEY    -1002

#define MEMC_RES_PAYLOAD_FAILURE -1001

#define MEMC_VAL_COMPRESSED    (1<<0)
#define MEMC_VAL_SERIALIZED    (1<<1)
#define MEMC_VAL_IS_LONG       (1<<2)
#define MEMC_VAL_IS_DOUBLE     (1<<3)

#define MEMC_METHOD_INIT_VARS              \
    zval*             object  = getThis(); \
    php_memc_t*       i_obj   = NULL;      \

#define MEMC_METHOD_FETCH_OBJECT                                               \
    i_obj = (php_memc_t *) zend_object_store_get_object( object TSRMLS_CC );   \

#ifndef DVAL_TO_LVAL
#ifdef _WIN64
# define DVAL_TO_LVAL(d, l) \
      if ((d) > LONG_MAX) { \
              (l) = (long)(unsigned long)(__int64) (d); \
      } else { \
              (l) = (long) (d); \
      }
#else
# define DVAL_TO_LVAL(d, l) \
      if ((d) > LONG_MAX) { \
              (l) = (unsigned long) (d); \
      } else { \
              (l) = (long) (d); \
      }
#endif
#endif

typedef struct {
	zend_object zo;

	memcached_st *memc;

	unsigned is_persistent:1;
	const char *plist_key;
	int plist_key_len;

	unsigned compression:1;
} php_memc_t;

enum {
	MEMC_OP_SET,
	MEMC_OP_ADD,
	MEMC_OP_REPLACE,
	MEMC_OP_APPEND,
	MEMC_OP_PREPEND
};

static int le_memc;

static zend_class_entry *memcached_ce = NULL;
static zend_class_entry *memcached_exception_ce = NULL;
static zend_class_entry *spl_ce_RuntimeException = NULL;

ZEND_DECLARE_MODULE_GLOBALS(php_memcached)

#ifdef COMPILE_DL_MEMCACHED
ZEND_GET_MODULE(memcached)
#endif

/****************************************
  Forward declarations
****************************************/
static int php_memc_list_entry(void);
static int php_memc_handle_error(memcached_return status TSRMLS_DC);
static char *php_memc_zval_to_payload(zval *value, size_t *payload_len, uint32_t *flags TSRMLS_DC);
static int php_memc_zval_from_payload(zval *value, char *payload, size_t payload_len, uint32_t flags TSRMLS_DC);
static void php_memc_get_impl(INTERNAL_FUNCTION_PARAMETERS, zend_bool by_key);
static void php_memc_getMulti_impl(INTERNAL_FUNCTION_PARAMETERS, zend_bool by_key);
static void php_memc_store_impl(INTERNAL_FUNCTION_PARAMETERS, int op, zend_bool by_key);
static void php_memc_setMulti_impl(INTERNAL_FUNCTION_PARAMETERS, zend_bool by_key);
static void php_memc_cas_impl(INTERNAL_FUNCTION_PARAMETERS, zend_bool by_key);
static void php_memc_delete_impl(INTERNAL_FUNCTION_PARAMETERS, zend_bool by_key);
static void php_memc_incdec_impl(INTERNAL_FUNCTION_PARAMETERS, zend_bool incr);
static void php_memc_getDelayed_impl(INTERNAL_FUNCTION_PARAMETERS, zend_bool by_key);


/****************************************
  Method implementations
****************************************/

/* {{{ Memcached::__construct() */
static PHP_METHOD(Memcached, __construct)
{
	zval *object = getThis();
	php_memc_t *i_obj;
	memcached_st *memc = NULL;
	char *persistent_id = NULL;
	int persistent_id_len;
	zend_bool skip_ctor = 0;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "|s", &persistent_id,
							  &persistent_id_len) == FAILURE) {
		ZVAL_NULL(object);
		return;
	}

	i_obj = (php_memc_t *) zend_object_store_get_object(object TSRMLS_CC);

	if (persistent_id) {
		char *plist_key = NULL;
		int plist_key_len = 0;
		zend_rsrc_list_entry *le;
		php_memc_t *pi_obj = NULL;

		plist_key_len = spprintf(&plist_key, 0, "memcached:id=%s", persistent_id);
		if (zend_hash_find(&EG(persistent_list), plist_key, plist_key_len+1, (void *)&le) == SUCCESS) {
			if (le->type == php_memc_list_entry()) {
				pi_obj = (php_memc_t *) le->ptr;
			}
		}

		if (pi_obj) {
			skip_ctor = 1;
		} else {
			pi_obj = pecalloc(1, sizeof(*pi_obj), 1);

			if (pi_obj == NULL) {
				php_error_docref(NULL TSRMLS_CC, E_ERROR, "out of memory: cannot allocate handle");
				/* not reached */
			}

			pi_obj->is_persistent = 1;
			if ((pi_obj->plist_key = pemalloc(plist_key_len + 1, 1)) == NULL) {
				php_error_docref(NULL TSRMLS_CC, E_ERROR, "out of memory: cannot allocate handle");
				/* not reached */
			}
			memcpy((char *)pi_obj->plist_key, plist_key, plist_key_len + 1);
			pi_obj->plist_key_len = plist_key_len + 1;

			/*
			 * Copy state bits because we've just constructed a new persistent object.
			 */
			pi_obj->compression = i_obj->compression;
		}

		/*
		 * Copy emalloc'ed bits.
		 */
		pi_obj->zo = i_obj->zo;

		/*
		 * Replace non-persistent object with the persistent one.
		 */
		efree(i_obj);
		i_obj = pi_obj;
		zend_object_store_set_object(object, i_obj TSRMLS_CC);

		if (plist_key) {
			efree(plist_key);
		}
	}

	if (skip_ctor) {
		return;
	}

	memc = memcached_create(NULL);
	if (memc == NULL) {
		php_error_docref(NULL TSRMLS_CC, E_ERROR, "could not allocate libmemcached structure");
		/* not reached */
	}
	i_obj->memc = memc;

	if (persistent_id) {
		zend_rsrc_list_entry le;

		le.type = php_memc_list_entry();
		le.ptr = i_obj;
		if (zend_hash_update(&EG(persistent_list), (char *)i_obj->plist_key,
							 i_obj->plist_key_len, (void *)&le, sizeof(le), NULL) == FAILURE) {
			php_error_docref(NULL TSRMLS_CC, E_ERROR, "could not register persistent entry");
			/* not reached */
		}
	}
}
/* }}} */

/* {{{ Memcached::get() */
PHP_METHOD(Memcached, get)
{
	php_memc_get_impl(INTERNAL_FUNCTION_PARAM_PASSTHRU, 0);
}
/* }}} */

/* {{{ Memcached::getByKey() */
PHP_METHOD(Memcached, getByKey)
{
	php_memc_get_impl(INTERNAL_FUNCTION_PARAM_PASSTHRU, 1);
}
/* }}} */

/* {{{ -- php_memc_get_impl */
static void php_memc_get_impl(INTERNAL_FUNCTION_PARAMETERS, zend_bool by_key)
{
	char *key = NULL;
	int   key_len = 0;
	char *server_key = NULL;
	int   server_key_len = 0;
	char  *payload = NULL;
	size_t payload_len = 0;
	uint32_t flags = 0;
	uint64_t cas = 0;
	zval *cas_token = NULL;
	memcached_result_st result;
	memcached_return status = MEMCACHED_SUCCESS;
	MEMC_METHOD_INIT_VARS;

	if (by_key) {
		if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "ss|z", &server_key,
								  &server_key_len, &key, &key_len, &cas_token) == FAILURE) {
			return;
		}
	} else {
		if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s|z", &key, &key_len,
								  &cas_token) == FAILURE) {
			return;
		}
	}

	MEMC_METHOD_FETCH_OBJECT;
	MEMC_G(rescode) = MEMCACHED_SUCCESS;

	if (cas_token) {

		uint64_t orig_cas_flag;

		orig_cas_flag = memcached_behavior_get(i_obj->memc, MEMCACHED_BEHAVIOR_SUPPORT_CAS);
		memcached_behavior_set(i_obj->memc, MEMCACHED_BEHAVIOR_SUPPORT_CAS, 1);

		status = memcached_mget_by_key(i_obj->memc, server_key, server_key_len, &key, &key_len, 1);

		if (php_memc_handle_error(status) < 0) {
			RETURN_FALSE;
		}

		status = MEMCACHED_SUCCESS;
		memcached_result_create(i_obj->memc, &result);

		if (memcached_fetch_result(i_obj->memc, &result, &status) == NULL
			&& php_memc_handle_error(status) < 0) {

			memcached_result_free(&result);
			RETURN_FALSE;
		}

		payload     = memcached_result_value(&result);
		payload_len = memcached_result_length(&result);
		flags       = memcached_result_flags(&result);
		cas         = memcached_result_cas(&result);

		if (php_memc_zval_from_payload(return_value, payload, payload_len, flags TSRMLS_CC) < 0) {
			memcached_result_free(&result);
			MEMC_G(rescode) = MEMC_RES_PAYLOAD_FAILURE;
			RETURN_FALSE;
		}

		zval_dtor(cas_token);
		ZVAL_DOUBLE(cas_token, (double)cas);

		memcached_result_free(&result);
		memcached_behavior_set(i_obj->memc, MEMCACHED_BEHAVIOR_SUPPORT_CAS, orig_cas_flag);
		return;

	} else {

		payload = memcached_get_by_key(i_obj->memc, server_key, server_key_len, key,
									   key_len, &payload_len, &flags, &status);
		if (php_memc_handle_error(status TSRMLS_CC) < 0) {
			RETURN_FALSE;
		}
		if (php_memc_zval_from_payload(return_value, payload, payload_len, flags TSRMLS_CC) < 0) {
			MEMC_G(rescode) = MEMC_RES_PAYLOAD_FAILURE;
			RETURN_FALSE;
		}

	}
}
/* }}} */

/* {{{ Memcached::getMulti() */
PHP_METHOD(Memcached, getMulti)
{
	php_memc_getMulti_impl(INTERNAL_FUNCTION_PARAM_PASSTHRU, 0);
}
/* }}} */

/* {{{ Memcached::getMultiByKey() */
PHP_METHOD(Memcached, getMultiByKey)
{
	php_memc_getMulti_impl(INTERNAL_FUNCTION_PARAM_PASSTHRU, 1);
}
/* }}} */

/* {{{ -- php_memc_getMulti_impl */
static void php_memc_getMulti_impl(INTERNAL_FUNCTION_PARAMETERS, zend_bool by_key)
{
	zval *keys = NULL;
	char *server_key = NULL;
	int   server_key_len = 0;
	size_t num_keys = 0;
	zval **entry = NULL;
	char  *payload = NULL;
	size_t payload_len = 0;
	char **mkeys = NULL;
	size_t *mkeys_len = NULL;
	char *res_key = NULL;
	size_t res_key_len = 0;
	uint32_t flags;
	uint64_t cas = 0;
	zval *cas_tokens = NULL;
	uint64_t orig_cas_flag;
	zval *value;
	int i = 0;
	memcached_result_st result;
	memcached_return status = MEMCACHED_SUCCESS;
	MEMC_METHOD_INIT_VARS;

	if (by_key) {
		if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "sa|z", &server_key,
								  &server_key_len, &keys, &cas_tokens) == FAILURE) {
			return;
		}
	} else {
		if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "a|z", &keys, &cas_tokens) == FAILURE) {
			return;
		}
	}

	MEMC_METHOD_FETCH_OBJECT;
	MEMC_G(rescode) = MEMCACHED_SUCCESS;

	num_keys  = zend_hash_num_elements(Z_ARRVAL_P(keys));
	mkeys     = safe_emalloc(num_keys, sizeof(char *), 0);
	mkeys_len = safe_emalloc(num_keys, sizeof(size_t), 0);

	for (zend_hash_internal_pointer_reset(Z_ARRVAL_P(keys));
		 zend_hash_get_current_data(Z_ARRVAL_P(keys), (void**)&entry) == SUCCESS;
		 zend_hash_move_forward(Z_ARRVAL_P(keys))) {

		if (Z_TYPE_PP(entry) == IS_STRING) {
			mkeys[i]     = Z_STRVAL_PP(entry);
			mkeys_len[i] = Z_STRLEN_PP(entry);
			i++;
		}
	}

	if (cas_tokens) {
		orig_cas_flag = memcached_behavior_get(i_obj->memc, MEMCACHED_BEHAVIOR_SUPPORT_CAS);
		memcached_behavior_set(i_obj->memc, MEMCACHED_BEHAVIOR_SUPPORT_CAS, 1);
	}

	status = memcached_mget_by_key(i_obj->memc, server_key, server_key_len, mkeys, mkeys_len, i);

	if (cas_tokens) {
		memcached_behavior_set(i_obj->memc, MEMCACHED_BEHAVIOR_SUPPORT_CAS, orig_cas_flag);
	}

	efree(mkeys);
	efree(mkeys_len);
	if (php_memc_handle_error(status) < 0) {
		RETURN_FALSE;
	}

	if (cas_tokens) {
		zval_dtor(cas_tokens);
		array_init(cas_tokens);
	}
	array_init(return_value);
	status = MEMCACHED_SUCCESS;
	memcached_result_create(i_obj->memc, &result);
	while ((memcached_fetch_result(i_obj->memc, &result, &status)) != NULL) {

		payload     = memcached_result_value(&result);
		payload_len = memcached_result_length(&result);
		flags       = memcached_result_flags(&result);
		res_key     = memcached_result_key_value(&result);
		res_key_len = memcached_result_key_length(&result);

		MAKE_STD_ZVAL(value);

		if (php_memc_zval_from_payload(value, payload, payload_len, flags TSRMLS_CC) < 0) {
			zval_ptr_dtor(&value);
			zval_dtor(return_value);
			MEMC_G(rescode) = MEMC_RES_PAYLOAD_FAILURE;
			RETURN_FALSE;
		}

		add_assoc_zval_ex(return_value, res_key, res_key_len+1, value);
		if (cas_tokens) {
			cas = memcached_result_cas(&result);
			add_assoc_double_ex(cas_tokens, res_key, res_key_len+1, (double)cas);
		}
	}

	memcached_result_free(&result);

	if (status != MEMCACHED_END && php_memc_handle_error(status) < 0) {
		zval_dtor(return_value);
		RETURN_FALSE;
	}
}
/* }}} */

/* {{{ Memcached::getDelayed() */
PHP_METHOD(Memcached, getDelayed)
{
	php_memc_getDelayed_impl(INTERNAL_FUNCTION_PARAM_PASSTHRU, 0);
}
/* }}} */

/* {{{ Memcached::getDelayedByKey() */
PHP_METHOD(Memcached, getDelayedByKey)
{
	php_memc_getDelayed_impl(INTERNAL_FUNCTION_PARAM_PASSTHRU, 1);
}
/* }}} */

/* {{{ php_memc_getDelayed_impl */
static void php_memc_getDelayed_impl(INTERNAL_FUNCTION_PARAMETERS, zend_bool by_key)
{
	zval *keys = NULL;
	char *server_key = NULL;
	int   server_key_len = 0;
	zend_bool with_cas = 0;
	size_t num_keys = 0;
	zval **entry = NULL;
	char **mkeys = NULL;
	size_t *mkeys_len = NULL;
	uint64_t orig_cas_flag;
	int i = 0;
	memcached_return status = MEMCACHED_SUCCESS;
	MEMC_METHOD_INIT_VARS;

	if (by_key) {
		if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "sa|b", &server_key,
								  &server_key_len, &keys, &with_cas) == FAILURE) {
			return;
		}
	} else {
		if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "a|b", &keys, &with_cas) == FAILURE) {
			return;
		}
	}

	MEMC_METHOD_FETCH_OBJECT;
	MEMC_G(rescode) = MEMCACHED_SUCCESS;

	num_keys  = zend_hash_num_elements(Z_ARRVAL_P(keys));
	mkeys     = safe_emalloc(num_keys, sizeof(char *), 0);
	mkeys_len = safe_emalloc(num_keys, sizeof(size_t), 0);

	for (zend_hash_internal_pointer_reset(Z_ARRVAL_P(keys));
		 zend_hash_get_current_data(Z_ARRVAL_P(keys), (void**)&entry) == SUCCESS;
		 zend_hash_move_forward(Z_ARRVAL_P(keys))) {

		if (Z_TYPE_PP(entry) == IS_STRING) {
			mkeys[i]     = Z_STRVAL_PP(entry);
			mkeys_len[i] = Z_STRLEN_PP(entry);
			i++;
		}
	}

	if (with_cas) {
		orig_cas_flag = memcached_behavior_get(i_obj->memc, MEMCACHED_BEHAVIOR_SUPPORT_CAS);
		memcached_behavior_set(i_obj->memc, MEMCACHED_BEHAVIOR_SUPPORT_CAS, 1);
	}

	status = memcached_mget_by_key(i_obj->memc, server_key, server_key_len, mkeys, mkeys_len, i);

	if (with_cas) {
		memcached_behavior_set(i_obj->memc, MEMCACHED_BEHAVIOR_SUPPORT_CAS, orig_cas_flag);
	}

	efree(mkeys);
	efree(mkeys_len);
	if (php_memc_handle_error(status) < 0) {
		RETURN_FALSE;
	}

	RETURN_TRUE;
}
/* }}} */

/* {{{ Memcached::fetch() */
PHP_METHOD(Memcached, fetch)
{
	char *res_key = NULL;
	size_t res_key_len = 0;
	char  *payload = NULL;
	size_t payload_len = 0;
	zval *value;
	uint32_t flags;
	uint64_t cas = 0;
	memcached_result_st result;
	memcached_return status = MEMCACHED_SUCCESS;
	MEMC_METHOD_INIT_VARS;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "") == FAILURE) {
		return;
	}

	MEMC_METHOD_FETCH_OBJECT;

	memcached_result_create(i_obj->memc, &result);
	if ((memcached_fetch_result(i_obj->memc, &result, &status)) == NULL) {
		php_memc_handle_error(status TSRMLS_CC);
		memcached_result_free(&result);
		RETURN_FALSE;
	}

	payload     = memcached_result_value(&result);
	payload_len = memcached_result_length(&result);
	flags       = memcached_result_flags(&result);
	res_key     = memcached_result_key_value(&result);
	res_key_len = memcached_result_key_length(&result);
	cas 		= memcached_result_cas(&result);

	MAKE_STD_ZVAL(value);

	if (php_memc_zval_from_payload(value, payload, payload_len, flags TSRMLS_CC) < 0) {
		zval_ptr_dtor(&value);
		MEMC_G(rescode) = MEMC_RES_PAYLOAD_FAILURE;
		RETURN_FALSE;
	}

	array_init(return_value);
	add_assoc_stringl_ex(return_value, ZEND_STRS("key"), res_key, res_key_len, 1);
	add_assoc_zval_ex(return_value, ZEND_STRS("value"), value);
	add_assoc_double_ex(return_value, ZEND_STRS("cas"), (double)cas);

	memcached_result_free(&result);
}
/* }}} */

/* {{{ Memcached::fetchAll() */
PHP_METHOD(Memcached, fetchAll)
{
	char *res_key = NULL;
	size_t res_key_len = 0;
	char  *payload = NULL;
	size_t payload_len = 0;
	zval *value, *entry;
	uint32_t flags;
	uint64_t cas = 0;
	memcached_result_st result;
	memcached_return status = MEMCACHED_SUCCESS;
	MEMC_METHOD_INIT_VARS;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "") == FAILURE) {
		return;
	}

	MEMC_METHOD_FETCH_OBJECT;

	array_init(return_value);
	memcached_result_create(i_obj->memc, &result);

	while ((memcached_fetch_result(i_obj->memc, &result, &status)) != NULL) {

		payload     = memcached_result_value(&result);
		payload_len = memcached_result_length(&result);
		flags       = memcached_result_flags(&result);
		res_key     = memcached_result_key_value(&result);
		res_key_len = memcached_result_key_length(&result);
		cas 		= memcached_result_cas(&result);

		MAKE_STD_ZVAL(value);

		if (php_memc_zval_from_payload(value, payload, payload_len, flags TSRMLS_CC) < 0) {
			zval_ptr_dtor(&value);
			zval_dtor(return_value);
			MEMC_G(rescode) = MEMC_RES_PAYLOAD_FAILURE;
			RETURN_FALSE;
		}

		MAKE_STD_ZVAL(entry);
		array_init(entry);
		add_assoc_stringl_ex(entry, ZEND_STRS("key"), res_key, res_key_len, 1);
		add_assoc_zval_ex(entry, ZEND_STRS("value"), value);
		add_assoc_double_ex(entry, ZEND_STRS("cas"), (double)cas);
		add_next_index_zval(return_value, entry);
	}

	memcached_result_free(&result);

	if (status != MEMCACHED_END && php_memc_handle_error(status) < 0) {
		zval_dtor(return_value);
		RETURN_FALSE;
	}
}
/* }}} */

/* {{{ Memcached::set() */
PHP_METHOD(Memcached, set)
{
	php_memc_store_impl(INTERNAL_FUNCTION_PARAM_PASSTHRU, MEMC_OP_SET, 0);
}
/* }}} */

/* {{{ Memcached::setByKey() */
PHP_METHOD(Memcached, setByKey)
{
	php_memc_store_impl(INTERNAL_FUNCTION_PARAM_PASSTHRU, MEMC_OP_SET, 1);
}
/* }}} */

/* {{{ Memcached::setMulti() */
PHP_METHOD(Memcached, setMulti)
{
	php_memc_setMulti_impl(INTERNAL_FUNCTION_PARAM_PASSTHRU, 0);
}
/* }}} */

/* {{{ Memcached::setMultiByKey() */
PHP_METHOD(Memcached, setMultiByKey)
{
	php_memc_setMulti_impl(INTERNAL_FUNCTION_PARAM_PASSTHRU, 1);
}
/* }}} */

/* {{{ -- php_memc_setMulti_impl */
static void php_memc_setMulti_impl(INTERNAL_FUNCTION_PARAMETERS, zend_bool by_key)
{
	zval *entries;
	char *server_key = NULL;
	int   server_key_len = 0;
	time_t expiration = 0;
	zval *value, **entry;
	char *str_key;
	uint  str_key_len;
	ulong num_key;
	char  *payload;
	size_t payload_len;
	uint32_t flags = 0;
	memcached_return status;
	MEMC_METHOD_INIT_VARS;

	if (by_key) {
		if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "sa|l", &server_key,
								  &server_key_len, &entries, &expiration) == FAILURE) {
			return;
		}
	} else {
		if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "a|l", &entries, &expiration) == FAILURE) {
			return;
		}
	}

	MEMC_METHOD_FETCH_OBJECT;

	for (zend_hash_internal_pointer_reset(Z_ARRVAL_P(entries));
		 zend_hash_get_current_data(Z_ARRVAL_P(entries), (void**)&entry) == SUCCESS;
		 zend_hash_move_forward(Z_ARRVAL_P(entries))) {

		if (zend_hash_get_current_key_ex(Z_ARRVAL_P(entries), &str_key, &str_key_len, &num_key, 0, NULL) != HASH_KEY_IS_STRING) {
			continue;
		}

		flags = 0;
		if (i_obj->compression) {
			flags |= MEMC_VAL_COMPRESSED;
		}

		payload = php_memc_zval_to_payload(*entry, &payload_len, &flags TSRMLS_CC);
		if (payload == NULL) {
			MEMC_G(rescode) = MEMC_RES_PAYLOAD_FAILURE;
			RETURN_FALSE;
		}

		if (!by_key) {
			server_key     = str_key;
			server_key_len = str_key_len;
		}
		status = memcached_set_by_key(i_obj->memc, server_key, server_key_len, str_key,
									  str_key_len, payload, payload_len, expiration, flags);
		efree(payload);

		if (php_memc_handle_error(status TSRMLS_CC) < 0) {
			RETURN_FALSE;
		}
	}

	RETURN_TRUE;
}
/* }}} */

/* {{{ Memcached::add() */
PHP_METHOD(Memcached, add)
{
	php_memc_store_impl(INTERNAL_FUNCTION_PARAM_PASSTHRU, MEMC_OP_ADD, 0);
}
/* }}} */

/* {{{ Memcached::addByKey() */
PHP_METHOD(Memcached, addByKey)
{
	php_memc_store_impl(INTERNAL_FUNCTION_PARAM_PASSTHRU, MEMC_OP_ADD, 1);
}
/* }}} */

/* {{{ Memcached::append() */
PHP_METHOD(Memcached, append)
{
	php_memc_store_impl(INTERNAL_FUNCTION_PARAM_PASSTHRU, MEMC_OP_APPEND, 0);
}
/* }}} */

/* {{{ Memcached::appendByKey() */
PHP_METHOD(Memcached, appendByKey)
{
	php_memc_store_impl(INTERNAL_FUNCTION_PARAM_PASSTHRU, MEMC_OP_APPEND, 1);
}
/* }}} */

/* {{{ Memcached::prepend() */
PHP_METHOD(Memcached, prepend)
{
	php_memc_store_impl(INTERNAL_FUNCTION_PARAM_PASSTHRU, MEMC_OP_PREPEND, 0);
}
/* }}} */

/* {{{ Memcached::prependByKey() */
PHP_METHOD(Memcached, prependByKey)
{
	php_memc_store_impl(INTERNAL_FUNCTION_PARAM_PASSTHRU, MEMC_OP_PREPEND, 1);
}
/* }}} */

/* {{{ Memcached::replace() */
PHP_METHOD(Memcached, replace)
{
	php_memc_store_impl(INTERNAL_FUNCTION_PARAM_PASSTHRU, MEMC_OP_REPLACE, 0);
}
/* }}} */

/* {{{ Memcached::replaceByKey() */
PHP_METHOD(Memcached, replaceByKey)
{
	php_memc_store_impl(INTERNAL_FUNCTION_PARAM_PASSTHRU, MEMC_OP_REPLACE, 1);
}
/* }}} */

/* {{{ -- php_memc_store_impl */
static void php_memc_store_impl(INTERNAL_FUNCTION_PARAMETERS, int op, zend_bool by_key)
{
	char *key = NULL;
	int   key_len = 0;
	char *server_key = NULL;
	int   server_key_len = 0;
	zval *value;
	time_t expiration = 0;
	char  *payload;
	size_t payload_len;
	uint32_t flags = 0;
	memcached_return status;
	MEMC_METHOD_INIT_VARS;

	if (by_key) {
		if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "ssz|l", &server_key,
								  &server_key_len, &key, &key_len, &value, &expiration) == FAILURE) {
			return;
		}
	} else {
		if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "sz|l", &key, &key_len,
								  &value, &expiration) == FAILURE) {
			return;
		}
		server_key     = key;
		server_key_len = key_len;
	}

	MEMC_METHOD_FETCH_OBJECT;

	if (i_obj->compression) {
		if (op == MEMC_OP_APPEND || op == MEMC_OP_PREPEND) {
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "cannot append/prepend with compression turned on");
			return;
		}
		flags |= MEMC_VAL_COMPRESSED;
	}

	payload = php_memc_zval_to_payload(value, &payload_len, &flags TSRMLS_CC);
	if (payload == NULL) {
		MEMC_G(rescode) = MEMC_RES_PAYLOAD_FAILURE;
		RETURN_FALSE;
	}

	switch (op) {
		case MEMC_OP_SET:
			status = memcached_set_by_key(i_obj->memc, server_key, server_key_len, key,
										  key_len, payload, payload_len, expiration, flags);
			break;

		case MEMC_OP_ADD:
			status = memcached_add_by_key(i_obj->memc, server_key, server_key_len, key,
										  key_len, payload, payload_len, expiration, flags);
			break;

		case MEMC_OP_REPLACE:
			status = memcached_replace_by_key(i_obj->memc, server_key, server_key_len, key,
										      key_len, payload, payload_len, expiration, flags);
			break;

		case MEMC_OP_APPEND:
			status = memcached_append_by_key(i_obj->memc, server_key, server_key_len, key,
											 key_len, payload, payload_len, expiration, flags);
			break;

		case MEMC_OP_PREPEND:
			status = memcached_prepend_by_key(i_obj->memc, server_key, server_key_len, key,
											  key_len, payload, payload_len, expiration, flags);
			break;

		default:
			/* not reached */
			assert(0);
			break;
	}

	efree(payload);
	if (php_memc_handle_error(status TSRMLS_CC) < 0) {
		RETURN_FALSE;
	}

	RETURN_TRUE;
}
/* }}} */

/* {{{ Memcached::cas() */
PHP_METHOD(Memcached, cas)
{
	php_memc_cas_impl(INTERNAL_FUNCTION_PARAM_PASSTHRU, 0);
}
/* }}} */

/* {{{ Memcached::casByKey() */
PHP_METHOD(Memcached, casByKey)
{
	php_memc_cas_impl(INTERNAL_FUNCTION_PARAM_PASSTHRU, 1);
}
/* }}} */

/* {{{ -- php_memc_cas_impl */
static void php_memc_cas_impl(INTERNAL_FUNCTION_PARAMETERS, zend_bool by_key)
{
	double cas_d;
	uint64_t cas;
	char *key = NULL;
	int   key_len = 0;
	char *server_key = NULL;
	int   server_key_len = 0;
	zval *value;
	time_t expiration = 0;
	char  *payload;
	size_t payload_len;
	uint32_t flags = 0;
	memcached_return status;
	MEMC_METHOD_INIT_VARS;

	if (by_key) {
		if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "dssz|l", &cas_d, &server_key,
								  &server_key_len, &key, &key_len, &value, &expiration) == FAILURE) {
			return;
		}
	} else {
		if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "dsz|l", &cas_d, &key, &key_len,
								  &value, &expiration) == FAILURE) {
			return;
		}
		server_key     = key;
		server_key_len = key_len;
	}

	MEMC_METHOD_FETCH_OBJECT;

	DVAL_TO_LVAL(cas_d, cas);

	if (i_obj->compression) {
		flags |= MEMC_VAL_COMPRESSED;
	}

	payload = php_memc_zval_to_payload(value, &payload_len, &flags TSRMLS_CC);
	if (payload == NULL) {
		MEMC_G(rescode) = MEMC_RES_PAYLOAD_FAILURE;
		RETURN_FALSE;
	}
	status = memcached_cas_by_key(i_obj->memc, server_key, server_key_len, key, key_len,
								  payload, payload_len, expiration, flags, cas);
	efree(payload);
	if (php_memc_handle_error(status TSRMLS_CC) < 0) {
		RETURN_FALSE;
	}

	RETURN_TRUE;
}
/* }}} */

/* {{{ Memcached::delete() */
PHP_METHOD(Memcached, delete)
{
	php_memc_delete_impl(INTERNAL_FUNCTION_PARAM_PASSTHRU, 0);
}
/* }}} */

/* {{{ Memcached::deleteByKey() */
PHP_METHOD(Memcached, deleteByKey)
{
	php_memc_delete_impl(INTERNAL_FUNCTION_PARAM_PASSTHRU, 1);
}
/* }}} */

/* {{{ -- php_memc_delete_impl */
static void php_memc_delete_impl(INTERNAL_FUNCTION_PARAMETERS, zend_bool by_key)
{
	char *key = NULL;
	int   key_len = 0;
	char *server_key = NULL;
	int   server_key_len = 0;
	time_t expiration = 0;
	memcached_return status;
	MEMC_METHOD_INIT_VARS;

	if (by_key) {
		if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "ss|l", &server_key,
								  &server_key_len, &key, &key_len, &expiration) == FAILURE) {
			return;
		}
	} else {
		if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s|l", &key, &key_len,
								  &expiration) == FAILURE) {
			return;
		}
		server_key     = key;
		server_key_len = key_len;
	}

	MEMC_METHOD_FETCH_OBJECT;

	status = memcached_delete_by_key(i_obj->memc, server_key, server_key_len, key,
									 key_len, expiration);

	if (php_memc_handle_error(status TSRMLS_CC) < 0) {
		RETURN_FALSE;
	}

	RETURN_TRUE;
}
/* }}} */

/* {{{ Memcached::increment() */
PHP_METHOD(Memcached, increment)
{
	php_memc_incdec_impl(INTERNAL_FUNCTION_PARAM_PASSTHRU, 1);
}
/* }}} */

/* {{{ Memcached::decrement() */
PHP_METHOD(Memcached, decrement)
{
	php_memc_incdec_impl(INTERNAL_FUNCTION_PARAM_PASSTHRU, 0);
}
/* }}} */

/* {{{ -- php_memc_incdec_impl */
static void php_memc_incdec_impl(INTERNAL_FUNCTION_PARAMETERS, zend_bool incr)
{
	char *key = NULL;
	int   key_len = 0;
	long  offset = 1;
	uint64_t value;
	memcached_return status;
	MEMC_METHOD_INIT_VARS;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s|l", &key, &key_len, &offset) == FAILURE) {
		return;
	}

	if (offset < 0) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "offset has to be > 0");
		RETURN_FALSE;
	}

	MEMC_METHOD_FETCH_OBJECT;

	if (incr) {
		status = memcached_increment(i_obj->memc, key, key_len, (unsigned int)offset, &value);
	} else {
		status = memcached_decrement(i_obj->memc, key, key_len, (unsigned int)offset, &value);
	}

	if (php_memc_handle_error(status TSRMLS_CC) < 0) {
		RETURN_FALSE;
	}

	RETURN_LONG((long)value);
}
/* }}} */

/* {{{ Memcached::addServer() */
PHP_METHOD(Memcached, addServer)
{
	char *host;
	int   host_len;
	long  port, weight = 0;
	MEMC_METHOD_INIT_VARS;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "sl|l", &host, &host_len,
							  &port, &weight) == FAILURE) {
		return;
	}

	MEMC_METHOD_FETCH_OBJECT;

	if (memcached_server_add_with_weight(i_obj->memc, host, port, weight) != MEMCACHED_SUCCESS) {
		RETURN_FALSE;
	}

	RETURN_TRUE;
}
/* }}} */

/* {{{ Memcached::getServerList() */
PHP_METHOD(Memcached, getServerList)
{
	memcached_server_st *servers;
	unsigned int i, servers_count;
	zval *array;
	MEMC_METHOD_INIT_VARS;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "") == FAILURE) {
		return;
	}

	MEMC_METHOD_FETCH_OBJECT;

	array_init(return_value);
	servers = memcached_server_list(i_obj->memc);
	servers_count = memcached_server_count(i_obj->memc);
	if (servers == NULL) {
		return;
	}

	for (i = 0; i < servers_count; i++) {
		MAKE_STD_ZVAL(array);
		array_init(array);
		add_assoc_string(array, "host", servers[i].hostname, 1);
		add_assoc_long(array, "port", servers[i].port);
		add_assoc_long(array, "weight", servers[i].weight);
		add_next_index_zval(return_value, array);
	}
}
/* }}} */

/* {{{ Memcached::getServerByKey() */
PHP_METHOD(Memcached, getServerByKey)
{
	char *server_key;
	int   server_key_len;
	memcached_server_st *server;
	memcached_return error;
	MEMC_METHOD_INIT_VARS;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s", &server_key, &server_key_len) == FAILURE) {
		return;
	}

	MEMC_METHOD_FETCH_OBJECT;

	server = memcached_server_by_key(i_obj->memc, server_key, server_key_len, &error);
	if (server == NULL) {
		php_memc_handle_error(error TSRMLS_CC);
		RETURN_FALSE;
	}

	array_init(return_value);
	add_assoc_string(return_value, "host", server->hostname, 1);
	add_assoc_long(return_value, "port", server->port);
	add_assoc_long(return_value, "weight", server->weight);
	memcached_server_free(server);
}
/* }}} */

/* {{{ Memcached::flush() */
static PHP_METHOD(Memcached, flush)
{
	time_t expiration = 0;
	memcached_return status;
	MEMC_METHOD_INIT_VARS;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "|l", &expiration) == FAILURE) {
		return;
	}

	MEMC_METHOD_FETCH_OBJECT;

	status = memcached_flush(i_obj->memc, expiration);
	if (php_memc_handle_error(status TSRMLS_CC) < 0) {
		RETURN_FALSE;
	}

	RETURN_TRUE;
}
/* }}} */

/* {{{ Memcached::getOption() */
static PHP_METHOD(Memcached, getOption)
{
	long option;
	uint64_t result;
	memcached_behavior flag;
	MEMC_METHOD_INIT_VARS;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "l", &option) == FAILURE) {
		return;
	}

	MEMC_METHOD_FETCH_OBJECT;

	switch (option) {
		case MEMC_OPT_COMPRESSION:
			RETURN_BOOL(i_obj->compression);

		case MEMC_OPT_PREFIX_KEY:
		{
			memcached_return retval;
			char *result;

			result = memcached_callback_get(i_obj->memc, MEMCACHED_CALLBACK_PREFIX_KEY, &retval);
			if (retval == MEMCACHED_SUCCESS) {
				RETURN_STRING(result, 1);
			} else {
				RETURN_EMPTY_STRING();
			}
		}

		case MEMCACHED_BEHAVIOR_SOCKET_SEND_SIZE:
		case MEMCACHED_BEHAVIOR_SOCKET_RECV_SIZE:
			if (memcached_server_count(i_obj->memc) == 0) {
				php_error_docref(NULL TSRMLS_CC, E_WARNING, "no servers defined");
				return;
			}

		default:
			/*
			 * Assume that it's a libmemcached behavior option.
			 */
			flag = (memcached_behavior) option;
			result = memcached_behavior_get(i_obj->memc, flag);
			RETURN_LONG((long)result);
	}
}
/* }}} */

/* {{{ Memcached::setOption() */
static PHP_METHOD(Memcached, setOption)
{
	long option;
	memcached_behavior flag;
	zval *value;
	MEMC_METHOD_INIT_VARS;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "lz/", &option, &value) == FAILURE) {
		return;
	}

	MEMC_METHOD_FETCH_OBJECT;

	switch (option) {
		case MEMC_OPT_COMPRESSION:
			convert_to_boolean(value);
			i_obj->compression = Z_BVAL_P(value);
			break;

		case MEMC_OPT_PREFIX_KEY:
			convert_to_string(value);
			if (memcached_callback_set(i_obj->memc, MEMCACHED_CALLBACK_PREFIX_KEY,
									   Z_STRVAL_P(value)) == MEMCACHED_BAD_KEY_PROVIDED) {
				php_error_docref(NULL TSRMLS_CC, E_WARNING, "bad key provided");
				RETURN_FALSE;
			}
			break;

		default:
			/*
			 * Assume that it's a libmemcached behavior option.
			 */
			flag = (memcached_behavior) option;
			convert_to_long(value);
			if (memcached_behavior_set(i_obj->memc, flag, (uint64_t)Z_LVAL_P(value)) == MEMCACHED_FAILURE) {
				php_error_docref(NULL TSRMLS_CC, E_WARNING, "error setting memcached option");
				RETURN_FALSE;
			}
			break;
	}

	RETURN_TRUE;
}
/* }}} */

/* {{{ Memcached::getResultCode() */
static PHP_METHOD(Memcached, getResultCode)
{
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "") == FAILURE) {
		return;
	}

	RETURN_LONG(MEMC_G(rescode));
}
/* }}} */


/****************************************
  Internal support code
****************************************/

/* {{{ constructor/destructor */
static void php_memc_destroy(php_memc_t *i_obj TSRMLS_DC)
{
	memcached_free(i_obj->memc);

	pefree(i_obj, i_obj->is_persistent);
}

static void php_memc_free_storage(php_memc_t *i_obj TSRMLS_DC)
{
	zend_object_std_dtor(&i_obj->zo TSRMLS_CC);

	if (!i_obj->is_persistent) {
		php_memc_destroy(i_obj TSRMLS_CC);
	}
}

zend_object_value php_memc_new(zend_class_entry *ce TSRMLS_DC)
{
    zend_object_value retval;
    php_memc_t *i_obj;
    zval *tmp;

    i_obj = ecalloc(1, sizeof(*i_obj));
	zend_object_std_init( &i_obj->zo, ce TSRMLS_CC );
    zend_hash_copy(i_obj->zo.properties, &ce->default_properties, (copy_ctor_func_t) zval_add_ref, (void *) &tmp, sizeof(zval *));

	i_obj->compression = 1;

    retval.handle = zend_objects_store_put(i_obj, (zend_objects_store_dtor_t)zend_objects_destroy_object, (zend_objects_free_object_storage_t)php_memc_free_storage, NULL TSRMLS_CC);
    retval.handlers = zend_get_std_object_handlers();

    return retval;
}

ZEND_RSRC_DTOR_FUNC(php_memc_dtor)
{
    if (rsrc->ptr) {
        php_memc_t *i_obj = (php_memc_t *)rsrc->ptr;
		php_memc_destroy(i_obj TSRMLS_CC);
        rsrc->ptr = NULL;
    }
}
/* }}} */

/* {{{ internal API functions */
static int php_memc_handle_error(memcached_return status TSRMLS_DC)
{
	int result = 0;

	switch (status) {
		case MEMCACHED_SUCCESS:
		case MEMCACHED_STORED:
		case MEMCACHED_DELETED:
		case MEMCACHED_STAT:
			result = 0;
			break;

		case MEMCACHED_END:
		case MEMCACHED_BUFFERED:
			MEMC_G(rescode) = status;
			result = 0;
			break;

		default:
			MEMC_G(rescode) = status;
			result = -1;
			break;
	}

	return result;
}

static char *php_memc_zval_to_payload(zval *value, size_t *payload_len, uint32_t *flags TSRMLS_DC)
{
	char *payload;
	smart_str buf = {0};

	switch (Z_TYPE_P(value)) {

		case IS_STRING:
			smart_str_appendl(&buf, Z_STRVAL_P(value), Z_STRLEN_P(value));
			break;

		case IS_LONG:
		case IS_DOUBLE:
		case IS_BOOL:
		{
			zval value_copy;

			value_copy = *value;
			zval_copy_ctor(&value_copy);
			convert_to_string(&value_copy);
			smart_str_appendl(&buf, Z_STRVAL(value_copy), Z_STRLEN(value_copy));
			zval_dtor(&value_copy);

			*flags &= ~MEMC_VAL_COMPRESSED;
			if (Z_TYPE_P(value) == IS_LONG) {
				*flags |= MEMC_VAL_IS_LONG;
			} else if (Z_TYPE_P(value) == IS_DOUBLE) {
				*flags |= MEMC_VAL_IS_DOUBLE;
			}
			break;
		}

		default:
		{
			php_serialize_data_t var_hash;

			PHP_VAR_SERIALIZE_INIT(var_hash);
			php_var_serialize(&buf, &value, &var_hash TSRMLS_CC);
			PHP_VAR_SERIALIZE_DESTROY(var_hash);

			if (!buf.c) {
				php_error_docref(NULL TSRMLS_CC, E_WARNING, "could not serialize value");
				smart_str_free(&buf);
				return NULL;
			}

			*flags |= MEMC_VAL_SERIALIZED;
			break;
		}
	}

	if (*flags & MEMC_VAL_COMPRESSED) {
		unsigned long payload_comp_len = buf.len + (buf.len / 500) + 25 + 1;
		char *payload_comp = emalloc(payload_comp_len);

		if (compress(payload_comp, &payload_comp_len, buf.c, buf.len) != Z_OK) {
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "could not compress value");
			free(payload_comp);
			smart_str_free(&buf);
			return NULL;
		}
		payload      = payload_comp;
		*payload_len = payload_comp_len;
		payload[*payload_len] = 0;
	} else {
		payload      = emalloc(buf.len + 1);
		*payload_len = buf.len;
		memcpy(payload, buf.c, buf.len);
		payload[buf.len] = 0;
	}

	smart_str_free(&buf);
	return payload;
}

static int php_memc_zval_from_payload(zval *value, char *payload, size_t payload_len, uint32_t flags TSRMLS_DC)
{
	if (payload == NULL) {
		return -1;
	}

	if (flags & MEMC_VAL_COMPRESSED) {
		/*
		   From gzuncompress().

		   zlib::uncompress() wants to know the output data length
		   if none was given as a parameter
		   we try from input length * 2 up to input length * 2^15
		   doubling it whenever it wasn't big enough
		   that should be eneugh for all real life cases
		 */
		unsigned int factor = 1, maxfactor = 16;
		unsigned long length;
		int status;
		char *buf = NULL;
		do {
			length = (unsigned long)payload_len * (1 << factor++);
			buf = erealloc(buf, length + 1);
			memset(buf, 0, length + 1);
			status = uncompress(buf, &length, payload, payload_len);
		} while ((status==Z_BUF_ERROR) && (factor < maxfactor));

        payload = emalloc(length + 1);
        memcpy(payload, buf, length);
        payload_len = length;
		payload[payload_len] = 0;
        efree(buf);
        if (status != Z_OK) {
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "could not uncompress value");
            efree(payload);
            return -1;
        }
	}

	if (flags & MEMC_VAL_SERIALIZED) {
		const char *payload_tmp = payload;
		php_unserialize_data_t var_hash;

		PHP_VAR_UNSERIALIZE_INIT(var_hash);
		if (!php_var_unserialize(&value, (const unsigned char **)&payload_tmp, payload_tmp + payload_len, &var_hash TSRMLS_CC)) {
			ZVAL_FALSE(value);
			PHP_VAR_UNSERIALIZE_DESTROY(var_hash);
			if (flags & MEMC_VAL_COMPRESSED) {
				efree(payload);
			}
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "could not unserialize value");
			return -1;
		}
		PHP_VAR_UNSERIALIZE_DESTROY(var_hash);
	} else {
		payload[payload_len] = 0;
		if (flags & MEMC_VAL_IS_LONG) {
			long lval = strtol(payload, NULL, 10);
			ZVAL_LONG(value, lval);
		} else if (flags & MEMC_VAL_IS_DOUBLE) {
			double dval = zend_strtod(payload, NULL);
			ZVAL_DOUBLE(value, dval);
		} else {
			ZVAL_STRINGL(value, payload, payload_len, 1);
		}
	}

    if (flags & MEMC_VAL_COMPRESSED) {
        efree(payload);
    }

	return 0;
}

static int php_memc_list_entry(void)
{
	return le_memc;
}

static void php_memc_init_globals(zend_php_memcached_globals *php_memcached_globals_p TSRMLS_DC)
{
	MEMC_G(rescode) = MEMCACHED_SUCCESS;
}

static void php_memc_destroy_globals(zend_php_memcached_globals *php_memcached_globals_p TSRMLS_DC)
{
}

PHP_MEMCACHED_API
zend_class_entry *php_memc_get_ce(void)
{
	return memcached_ce;
}

PHP_MEMCACHED_API
zend_class_entry *php_memc_get_exception(void)
{
	return memcached_exception_ce;
}

PHP_MEMCACHED_API
zend_class_entry *php_memc_get_exception_base(int root TSRMLS_DC)
{
#if HAVE_SPL
	if (!root) {
		if (!spl_ce_RuntimeException) {
			zend_class_entry **pce;

			if (zend_hash_find(CG(class_table), "runtimeexception",
							   sizeof("RuntimeException"), (void **) &pce) == SUCCESS) {
				spl_ce_RuntimeException = *pce;
				return *pce;
			}
		} else {
			return spl_ce_RuntimeException;
		}
	}
#endif
#if (PHP_MAJOR_VERSION == 5) && (PHP_MINOR_VERSION < 2)
	return zend_exception_get_default();
#else
	return zend_exception_get_default(TSRMLS_C);
#endif
}
/* }}} */

/* {{{ methods arginfo */
static
ZEND_BEGIN_ARG_INFO_EX(arginfo___construct, 0, 0, 0)
	ZEND_ARG_INFO(0, persistent_id)
ZEND_END_ARG_INFO()

static
ZEND_BEGIN_ARG_INFO(arginfo_getResultCode, 0)
ZEND_END_ARG_INFO()

static
ZEND_BEGIN_ARG_INFO_EX(arginfo_get, 0, 0, 1)
	ZEND_ARG_INFO(0, key)
	ZEND_ARG_INFO(1, cas_token)
	ZEND_ARG_INFO(0, cache_cb)
ZEND_END_ARG_INFO()

static
ZEND_BEGIN_ARG_INFO_EX(arginfo_getByKey, 0, 0, 2)
	ZEND_ARG_INFO(0, server_key)
	ZEND_ARG_INFO(0, key)
	ZEND_ARG_INFO(1, cas_token)
	ZEND_ARG_INFO(0, cache_cb)
ZEND_END_ARG_INFO()

static
ZEND_BEGIN_ARG_INFO_EX(arginfo_getMulti, 0, 0, 1)
	ZEND_ARG_ARRAY_INFO(0, keys, 0)
	ZEND_ARG_INFO(1, cas_tokens)
	ZEND_ARG_INFO(0, cache_cb)
ZEND_END_ARG_INFO()

static
ZEND_BEGIN_ARG_INFO_EX(arginfo_getMultiByKey, 0, 0, 2)
	ZEND_ARG_INFO(0, server_key)
	ZEND_ARG_ARRAY_INFO(0, keys, 0)
	ZEND_ARG_INFO(1, cas_tokens)
	ZEND_ARG_INFO(0, cache_cb)
ZEND_END_ARG_INFO()

static
ZEND_BEGIN_ARG_INFO_EX(arginfo_getDelayed, 0, 0, 1)
	ZEND_ARG_ARRAY_INFO(0, keys, 0)
	ZEND_ARG_INFO(0, with_cas)
	ZEND_ARG_INFO(0, cache_cb)
	ZEND_ARG_INFO(0, value_cb)
ZEND_END_ARG_INFO()

static
ZEND_BEGIN_ARG_INFO_EX(arginfo_getDelayedByKey, 0, 0, 2)
	ZEND_ARG_INFO(0, server_key)
	ZEND_ARG_ARRAY_INFO(0, keys, 0)
	ZEND_ARG_INFO(0, cas_tokens)
	ZEND_ARG_INFO(0, cache_cb)
ZEND_END_ARG_INFO()

static
ZEND_BEGIN_ARG_INFO(arginfo_fetch, 0)
ZEND_END_ARG_INFO()

static
ZEND_BEGIN_ARG_INFO(arginfo_fetchAll, 0)
ZEND_END_ARG_INFO()

static
ZEND_BEGIN_ARG_INFO_EX(arginfo_set, 0, 0, 2)
	ZEND_ARG_INFO(0, key)
	ZEND_ARG_INFO(0, value)
	ZEND_ARG_INFO(0, expiration)
ZEND_END_ARG_INFO()

static
ZEND_BEGIN_ARG_INFO_EX(arginfo_setByKey, 0, 0, 3)
	ZEND_ARG_INFO(0, server_key)
	ZEND_ARG_INFO(0, key)
	ZEND_ARG_INFO(0, value)
	ZEND_ARG_INFO(0, expiration)
ZEND_END_ARG_INFO()

static
ZEND_BEGIN_ARG_INFO_EX(arginfo_setMulti, 0, 0, 1)
	ZEND_ARG_ARRAY_INFO(0, entries, 0)
	ZEND_ARG_INFO(0, expiration)
ZEND_END_ARG_INFO()

static
ZEND_BEGIN_ARG_INFO_EX(arginfo_setMultiByKey, 0, 0, 2)
	ZEND_ARG_INFO(0, server_key)
	ZEND_ARG_ARRAY_INFO(0, entries, 0)
	ZEND_ARG_INFO(0, expiration)
ZEND_END_ARG_INFO()

static
ZEND_BEGIN_ARG_INFO_EX(arginfo_add, 0, 0, 2)
	ZEND_ARG_INFO(0, key)
	ZEND_ARG_INFO(0, value)
	ZEND_ARG_INFO(0, expiration)
ZEND_END_ARG_INFO()

static
ZEND_BEGIN_ARG_INFO_EX(arginfo_addByKey, 0, 0, 3)
	ZEND_ARG_INFO(0, server_key)
	ZEND_ARG_INFO(0, key)
	ZEND_ARG_INFO(0, value)
	ZEND_ARG_INFO(0, expiration)
ZEND_END_ARG_INFO()

static
ZEND_BEGIN_ARG_INFO_EX(arginfo_replace, 0, 0, 2)
	ZEND_ARG_INFO(0, key)
	ZEND_ARG_INFO(0, value)
	ZEND_ARG_INFO(0, expiration)
ZEND_END_ARG_INFO()

static
ZEND_BEGIN_ARG_INFO_EX(arginfo_replaceByKey, 0, 0, 3)
	ZEND_ARG_INFO(0, server_key)
	ZEND_ARG_INFO(0, key)
	ZEND_ARG_INFO(0, value)
	ZEND_ARG_INFO(0, expiration)
ZEND_END_ARG_INFO()

static
ZEND_BEGIN_ARG_INFO_EX(arginfo_append, 0, 0, 2)
	ZEND_ARG_INFO(0, key)
	ZEND_ARG_INFO(0, value)
	ZEND_ARG_INFO(0, expiration)
ZEND_END_ARG_INFO()

static
ZEND_BEGIN_ARG_INFO_EX(arginfo_appendByKey, 0, 0, 3)
	ZEND_ARG_INFO(0, server_key)
	ZEND_ARG_INFO(0, key)
	ZEND_ARG_INFO(0, value)
	ZEND_ARG_INFO(0, expiration)
ZEND_END_ARG_INFO()

static
ZEND_BEGIN_ARG_INFO_EX(arginfo_prepend, 0, 0, 2)
	ZEND_ARG_INFO(0, key)
	ZEND_ARG_INFO(0, value)
	ZEND_ARG_INFO(0, expiration)
ZEND_END_ARG_INFO()

static
ZEND_BEGIN_ARG_INFO_EX(arginfo_prependByKey, 0, 0, 3)
	ZEND_ARG_INFO(0, server_key)
	ZEND_ARG_INFO(0, key)
	ZEND_ARG_INFO(0, value)
	ZEND_ARG_INFO(0, expiration)
ZEND_END_ARG_INFO()

static
ZEND_BEGIN_ARG_INFO_EX(arginfo_cas, 0, 0, 3)
	ZEND_ARG_INFO(0, cas_token)
	ZEND_ARG_INFO(0, key)
	ZEND_ARG_INFO(0, value)
	ZEND_ARG_INFO(0, expiration)
ZEND_END_ARG_INFO()

static
ZEND_BEGIN_ARG_INFO_EX(arginfo_casByKey, 0, 0, 4)
	ZEND_ARG_INFO(0, cas_token)
	ZEND_ARG_INFO(0, server_key)
	ZEND_ARG_INFO(0, key)
	ZEND_ARG_INFO(0, value)
	ZEND_ARG_INFO(0, expiration)
ZEND_END_ARG_INFO()

static
ZEND_BEGIN_ARG_INFO_EX(arginfo_delete, 0, 0, 1)
	ZEND_ARG_INFO(0, key)
	ZEND_ARG_INFO(0, expiration)
ZEND_END_ARG_INFO()

static
ZEND_BEGIN_ARG_INFO_EX(arginfo_deleteByKey, 0, 0, 2)
	ZEND_ARG_INFO(0, server_key)
	ZEND_ARG_INFO(0, key)
	ZEND_ARG_INFO(0, expiration)
ZEND_END_ARG_INFO()

static
ZEND_BEGIN_ARG_INFO_EX(arginfo_increment, 0, 0, 1)
	ZEND_ARG_INFO(0, key)
	ZEND_ARG_INFO(0, offset)
ZEND_END_ARG_INFO()

static
ZEND_BEGIN_ARG_INFO_EX(arginfo_decrement, 0, 0, 1)
	ZEND_ARG_INFO(0, key)
	ZEND_ARG_INFO(0, offset)
ZEND_END_ARG_INFO()

static
ZEND_BEGIN_ARG_INFO_EX(arginfo_flush, 0, 0, 0)
	ZEND_ARG_INFO(0, expiration)
ZEND_END_ARG_INFO()

static
ZEND_BEGIN_ARG_INFO_EX(arginfo_addServer, 0, 0, 2)
	ZEND_ARG_INFO(0, host)
	ZEND_ARG_INFO(0, port)
	ZEND_ARG_INFO(0, weight)
ZEND_END_ARG_INFO()

static
ZEND_BEGIN_ARG_INFO(arginfo_getServerList, 0)
ZEND_END_ARG_INFO()

static
ZEND_BEGIN_ARG_INFO(arginfo_getServerByKey, 0)
	ZEND_ARG_INFO(0, server_key)
ZEND_END_ARG_INFO()

static
ZEND_BEGIN_ARG_INFO(arginfo_getOption, 0)
	ZEND_ARG_INFO(0, option)
ZEND_END_ARG_INFO()

static
ZEND_BEGIN_ARG_INFO(arginfo_setOption, 0)
	ZEND_ARG_INFO(0, option)
	ZEND_ARG_INFO(0, value)
ZEND_END_ARG_INFO()
/* }}} */

/* {{{ memcached_class_methods */
#define MEMC_ME(name, args) PHP_ME(Memcached, name, args, ZEND_ACC_PUBLIC)
static zend_function_entry memcached_class_methods[] = {
    MEMC_ME(__construct,        arginfo___construct)

    MEMC_ME(getResultCode,      arginfo_getResultCode)

    MEMC_ME(get,                arginfo_get)
    MEMC_ME(getByKey,           arginfo_getByKey)
    MEMC_ME(getMulti,           arginfo_getMulti)
    MEMC_ME(getMultiByKey,      arginfo_getMultiByKey)
    MEMC_ME(getDelayed,         arginfo_getDelayed)
    MEMC_ME(getDelayedByKey,    arginfo_getDelayedByKey)
    MEMC_ME(fetch,              arginfo_fetch)
    MEMC_ME(fetchAll,           arginfo_fetchAll)

    MEMC_ME(set,                arginfo_set)
    MEMC_ME(setByKey,           arginfo_setByKey)
    MEMC_ME(setMulti,           arginfo_setMulti)
    MEMC_ME(setMultiByKey,      arginfo_setMultiByKey)

    MEMC_ME(cas,                arginfo_cas)
    MEMC_ME(casByKey,           arginfo_casByKey)
    MEMC_ME(add,                arginfo_add)
    MEMC_ME(addByKey,           arginfo_addByKey)
    MEMC_ME(append,             arginfo_append)
    MEMC_ME(appendByKey,        arginfo_appendByKey)
    MEMC_ME(prepend,            arginfo_prepend)
    MEMC_ME(prependByKey,       arginfo_prependByKey)
    MEMC_ME(replace,            arginfo_replace)
    MEMC_ME(replaceByKey,       arginfo_replaceByKey)
    MEMC_ME(delete,             arginfo_delete)
    MEMC_ME(deleteByKey,        arginfo_deleteByKey)

    MEMC_ME(increment,          arginfo_increment)
    MEMC_ME(decrement,          arginfo_decrement)

    MEMC_ME(addServer,          arginfo_addServer)
    MEMC_ME(getServerList,      arginfo_getServerList)
    MEMC_ME(getServerByKey,     arginfo_getServerByKey)

    MEMC_ME(flush,              arginfo_flush)

    MEMC_ME(getOption,          arginfo_getOption)
    MEMC_ME(setOption,          arginfo_setOption)
    { NULL, NULL, NULL }
};
#undef MEMC_ME
/* }}} */

/* {{{ memcached_module_entry
 */

#if ZEND_MODULE_API_NO >= 20050922
static const zend_module_dep memcached_deps[] = {
#ifdef HAVE_SPL
    ZEND_MOD_REQUIRED("spl")
#endif
    {NULL, NULL, NULL}
};
#endif

zend_module_entry memcached_module_entry = {
#if ZEND_MODULE_API_NO >= 20050922
    STANDARD_MODULE_HEADER_EX, NULL,
    (zend_module_dep*)memcached_deps,
#else
    STANDARD_MODULE_HEADER,
#endif
	"memcached",
	NULL,
	PHP_MINIT(memcached),
	PHP_MSHUTDOWN(memcached),
	NULL,
	NULL,
	PHP_MINFO(memcached),
	PHP_MEMCACHED_VERSION,
	STANDARD_MODULE_PROPERTIES
};
/* }}} */

/* {{{ php_memc_register_constants */
static void php_memc_register_constants(INIT_FUNC_ARGS)
{
	#define REGISTER_MEMC_CLASS_CONST_LONG(name, value) zend_declare_class_constant_long(php_memc_get_ce() , ZEND_STRS( #name ) - 1, value TSRMLS_CC)

	/*
	 * Class options
	 */

	REGISTER_MEMC_CLASS_CONST_LONG(OPT_COMPRESSION, MEMC_OPT_COMPRESSION);
	REGISTER_MEMC_CLASS_CONST_LONG(OPT_PREFIX_KEY,  MEMC_OPT_PREFIX_KEY);

	/*
	 * libmemcached behavior options
	 */

	REGISTER_MEMC_CLASS_CONST_LONG(OPT_HASH, MEMCACHED_BEHAVIOR_HASH);
	REGISTER_MEMC_CLASS_CONST_LONG(HASH_DEFAULT, MEMCACHED_HASH_DEFAULT);
	REGISTER_MEMC_CLASS_CONST_LONG(HASH_MD5, MEMCACHED_HASH_MD5);
	REGISTER_MEMC_CLASS_CONST_LONG(HASH_CRC, MEMCACHED_HASH_CRC);
	REGISTER_MEMC_CLASS_CONST_LONG(HASH_FNV1_64, MEMCACHED_HASH_FNV1_64);
	REGISTER_MEMC_CLASS_CONST_LONG(HASH_FNV1A_64, MEMCACHED_HASH_FNV1A_64);
	REGISTER_MEMC_CLASS_CONST_LONG(HASH_FNV1_32, MEMCACHED_HASH_FNV1_32);
	REGISTER_MEMC_CLASS_CONST_LONG(HASH_FNV1A_32, MEMCACHED_HASH_FNV1A_32);
	REGISTER_MEMC_CLASS_CONST_LONG(HASH_HSIEH, MEMCACHED_HASH_HSIEH);
	REGISTER_MEMC_CLASS_CONST_LONG(HASH_MURMUR, MEMCACHED_HASH_MURMUR);
	REGISTER_MEMC_CLASS_CONST_LONG(OPT_DISTRIBUTION, MEMCACHED_BEHAVIOR_DISTRIBUTION);
	REGISTER_MEMC_CLASS_CONST_LONG(DISTRIBUTION_MODULA, MEMCACHED_DISTRIBUTION_MODULA);
	REGISTER_MEMC_CLASS_CONST_LONG(DISTRIBUTION_CONSISTENT, MEMCACHED_DISTRIBUTION_CONSISTENT);
	REGISTER_MEMC_CLASS_CONST_LONG(OPT_BUFFER_WRITES, MEMCACHED_BEHAVIOR_BUFFER_REQUESTS);
	REGISTER_MEMC_CLASS_CONST_LONG(OPT_BINARY_PROTOCOL, MEMCACHED_BEHAVIOR_BINARY_PROTOCOL);
	REGISTER_MEMC_CLASS_CONST_LONG(OPT_NO_BLOCK, MEMCACHED_BEHAVIOR_NO_BLOCK);
	REGISTER_MEMC_CLASS_CONST_LONG(OPT_TCP_NODELAY, MEMCACHED_BEHAVIOR_TCP_NODELAY);
	REGISTER_MEMC_CLASS_CONST_LONG(OPT_SOCKET_SEND_SIZE, MEMCACHED_BEHAVIOR_SOCKET_SEND_SIZE);
	REGISTER_MEMC_CLASS_CONST_LONG(OPT_SOCKET_RECV_SIZE, MEMCACHED_BEHAVIOR_SOCKET_RECV_SIZE);
	REGISTER_MEMC_CLASS_CONST_LONG(OPT_CONNECT_TIMEOUT, MEMCACHED_BEHAVIOR_CONNECT_TIMEOUT);
	REGISTER_MEMC_CLASS_CONST_LONG(OPT_RETRY_TIMEOUT, MEMCACHED_BEHAVIOR_RETRY_TIMEOUT);
	REGISTER_MEMC_CLASS_CONST_LONG(OPT_SEND_TIMEOUT, MEMCACHED_BEHAVIOR_SND_TIMEOUT);
	REGISTER_MEMC_CLASS_CONST_LONG(OPT_RECV_TIMEOUT, MEMCACHED_BEHAVIOR_RETRY_TIMEOUT);
	REGISTER_MEMC_CLASS_CONST_LONG(OPT_POLL_TIMEOUT, MEMCACHED_BEHAVIOR_POLL_TIMEOUT);
	REGISTER_MEMC_CLASS_CONST_LONG(OPT_CACHE_LOOKUPS, MEMCACHED_BEHAVIOR_CACHE_LOOKUPS);
	REGISTER_MEMC_CLASS_CONST_LONG(OPT_SERVER_FAILURE_LIMIT, MEMCACHED_BEHAVIOR_SERVER_FAILURE_LIMIT);

	/*
	 * libmemcached result codes
	 */

    REGISTER_MEMC_CLASS_CONST_LONG(RES_SUCCESS,              MEMCACHED_SUCCESS);
    REGISTER_MEMC_CLASS_CONST_LONG(RES_FAILURE,              MEMCACHED_FAILURE);
    REGISTER_MEMC_CLASS_CONST_LONG(RES_HOST_LOOKUP_FAILURE,  MEMCACHED_HOST_LOOKUP_FAILURE);
    REGISTER_MEMC_CLASS_CONST_LONG(RES_UNKNOWN_READ_FAILURE, MEMCACHED_UNKNOWN_READ_FAILURE);
    REGISTER_MEMC_CLASS_CONST_LONG(RES_PROTOCOL_ERROR,       MEMCACHED_PROTOCOL_ERROR);
    REGISTER_MEMC_CLASS_CONST_LONG(RES_CLIENT_ERROR,         MEMCACHED_CLIENT_ERROR);
    REGISTER_MEMC_CLASS_CONST_LONG(RES_SERVER_ERROR,         MEMCACHED_SERVER_ERROR);
    REGISTER_MEMC_CLASS_CONST_LONG(RES_WRITE_FAILURE,        MEMCACHED_WRITE_FAILURE);
    REGISTER_MEMC_CLASS_CONST_LONG(RES_DATA_EXISTS,          MEMCACHED_DATA_EXISTS);
    REGISTER_MEMC_CLASS_CONST_LONG(RES_NOTSTORED,            MEMCACHED_NOTSTORED);
    REGISTER_MEMC_CLASS_CONST_LONG(RES_NOTFOUND,             MEMCACHED_NOTFOUND);
    REGISTER_MEMC_CLASS_CONST_LONG(RES_PARTIAL_READ,         MEMCACHED_PARTIAL_READ);
    REGISTER_MEMC_CLASS_CONST_LONG(RES_SOME_ERRORS,          MEMCACHED_SOME_ERRORS);
    REGISTER_MEMC_CLASS_CONST_LONG(RES_NO_SERVERS,           MEMCACHED_NO_SERVERS);
    REGISTER_MEMC_CLASS_CONST_LONG(RES_END,                  MEMCACHED_END);
    REGISTER_MEMC_CLASS_CONST_LONG(RES_ERRNO,                MEMCACHED_ERRNO);
    REGISTER_MEMC_CLASS_CONST_LONG(RES_BUFFERED,             MEMCACHED_BUFFERED);
    REGISTER_MEMC_CLASS_CONST_LONG(RES_TIMEOUT,              MEMCACHED_TIMEOUT);
    REGISTER_MEMC_CLASS_CONST_LONG(RES_BAD_KEY_PROVIDED,     MEMCACHED_BAD_KEY_PROVIDED);
    REGISTER_MEMC_CLASS_CONST_LONG(RES_CONNECTION_SOCKET_CREATE_FAILURE, MEMCACHED_CONNECTION_SOCKET_CREATE_FAILURE);

	/*
	 * Our result codes.
	 */

    REGISTER_MEMC_CLASS_CONST_LONG(RES_PAYLOAD_FAILURE, MEMC_RES_PAYLOAD_FAILURE);


	#undef REGISTER_MEMC_CLASS_CONST_LONG
}
/* }}} */

/* {{{ PHP_MINIT_FUNCTION */
PHP_MINIT_FUNCTION(memcached)
{
	zend_class_entry ce;

    le_memc = zend_register_list_destructors_ex(NULL, php_memc_dtor, "Memcached persistent connection", module_number);

	INIT_CLASS_ENTRY(ce, "Memcached", memcached_class_methods);
	memcached_ce = zend_register_internal_class(&ce TSRMLS_CC);
	memcached_ce->create_object = php_memc_new;

    INIT_CLASS_ENTRY(ce, "MemcachedException", NULL);
	memcached_exception_ce = zend_register_internal_class_ex(&ce, php_memc_get_exception_base(0 TSRMLS_CC), NULL TSRMLS_CC);
	/* TODO
	 * possibly declare custom exception property here
	 */

	php_memc_register_constants(INIT_FUNC_ARGS_PASSTHRU);

#ifdef ZTS
	ts_allocate_id(&php_memcached_globals_id, sizeof(zend_php_memcached_globals),
				   (ts_allocate_ctor) php_memc_init_globals, (ts_allocate_dtor) php_memc_destroy_globals);
#else
	php_memc_init_globals(&php_memcached_globals TSRMLS_CC);
#endif

	return SUCCESS;
}
/* }}} */

/* {{{ PHP_MSHUTDOWN_FUNCTION */
PHP_MSHUTDOWN_FUNCTION(memcached)
{
#ifdef ZTS
    ts_free_id(php_memcached_globals_id);
#else
    php_memc_destroy_globals(&php_memcached_globals TSRMLS_CC);
#endif

	return SUCCESS;
}
/* }}} */

/* {{{ PHP_MINFO_FUNCTION */
PHP_MINFO_FUNCTION(memcached)
{
	php_info_print_table_start();
	php_info_print_table_header(2, "memcached support", "enabled");
	php_info_print_table_row(2, "Version", PHP_MEMCACHED_VERSION);
	php_info_print_table_row(2, "Revision", "$Revision: 1.107 $");
	php_info_print_table_end();
}
/* }}} */


/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim: noet sw=4 ts=4 fdm=marker:
 */