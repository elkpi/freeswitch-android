/*
 * FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 * Copyright (C) 2005-2012, Anthony Minessale II <anthm@freeswitch.org>
 *
 * Version: MPL 1.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 *
 * The Initial Developer of the Original Code is
 * Anthony Minessale II <anthm@freeswitch.org>
 * Portions created by the Initial Developer are Copyright (C)
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 *
 * Karl Anderson <karl@2600hz.com>
 * Darren Schreiber <darren@2600hz.com>
 *
 *
 * kazoo_commands.c -- clones of mod_commands commands slightly modified for kazoo
 *
 */
#include "mod_kazoo.h"
#include <switch_curl.h>

#define UUID_SET_DESC "Set a variable"
#define UUID_SET_SYNTAX "<uuid> <var> [value]"

#define UUID_MULTISET_DESC "Set multiple variables"
#define UUID_MULTISET_SYNTAX "<uuid> <var>=<value>;<var>=<value>..."

#define KZ_HTTP_PUT_DESC "upload a local freeswitch file to a url"
#define KZ_HTTP_PUT_SYNTAX "localfile url"

SWITCH_STANDARD_API(uuid_setvar_function) {
	switch_core_session_t *psession = NULL;
	char *mycmd = NULL, *argv[3] = { 0 };
	int argc = 0;

	if (!zstr(cmd) && (mycmd = strdup(cmd))) {
		argc = switch_separate_string(mycmd, ' ', argv, (sizeof(argv) / sizeof(argv[0])));
		if ((argc == 2 || argc == 3) && !zstr(argv[0])) {
			char *uuid = argv[0];
			char *var_name = argv[1];
			char *var_value = NULL;

			if (argc == 3) {
				var_value = argv[2];
			}

			if ((psession = switch_core_session_locate(uuid))) {
				switch_channel_t *channel;
				switch_event_t *event;
				channel = switch_core_session_get_channel(psession);

				if (zstr(var_name)) {
					switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "No variable name specified.\n");
					stream->write_function(stream, "-ERR No variable specified\n");
				} else {
					switch_channel_set_variable(channel, var_name, var_value);
					stream->write_function(stream, "+OK\n");
				}

				if (switch_event_create(&event, SWITCH_EVENT_CHANNEL_DATA) == SWITCH_STATUS_SUCCESS) {
					switch_channel_event_set_data(channel, event);
					switch_event_fire(&event);
				}

				switch_core_session_rwunlock(psession);

			} else {
				stream->write_function(stream, "-ERR No such channel!\n");
			}
			goto done;
		}
	}

	stream->write_function(stream, "-USAGE: %s\n", UUID_SET_SYNTAX);

  done:
	switch_safe_free(mycmd);
	return SWITCH_STATUS_SUCCESS;
}

SWITCH_STANDARD_API(uuid_setvar_multi_function) {
	switch_core_session_t *psession = NULL;
	char *mycmd = NULL, *vars, *argv[64] = { 0 };
	int argc = 0;
	char *var_name, *var_value = NULL;

	if (!zstr(cmd) && (mycmd = strdup(cmd))) {
		char *uuid = mycmd;
		if (!(vars = strchr(uuid, ' '))) {
			goto done;
		}
		*vars++ = '\0';

		if ((psession = switch_core_session_locate(uuid))) {
			switch_channel_t *channel = switch_core_session_get_channel(psession);
			switch_event_t *event;
			int x, y = 0;
			argc = switch_separate_string(vars, ';', argv, (sizeof(argv) / sizeof(argv[0])));

			for (x = 0; x < argc; x++) {
				var_name = argv[x];
				if (var_name && (var_value = strchr(var_name, '='))) {
					*var_value++ = '\0';
				}
				if (zstr(var_name)) {
					switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "No variable name specified.\n");
					stream->write_function(stream, "-ERR No variable specified\n");
				} else {
					switch_channel_set_variable(channel, var_name, var_value);
					if (!strcasecmp(var_name, "effective_callee_id_number")) {
						switch_channel_set_profile_var(channel, "callee_id_number", var_value);
					} else if (!strcasecmp(var_name, "effective_callee_id_name")) {
						switch_channel_set_profile_var(channel, "callee_id_name", var_value);
					} else if (!strcasecmp(var_name, "effective_caller_id_number")) {
						switch_channel_set_profile_var(channel, "caller_id_number", var_value);
					} else if (!strcasecmp(var_name, "effective_caller_id_name")) {
						switch_channel_set_profile_var(channel, "caller_id_name", var_value);
					};

					y++;
				}
			}

                        /* keep kazoo nodes in sync */
			if (switch_event_create(&event, SWITCH_EVENT_CHANNEL_DATA) == SWITCH_STATUS_SUCCESS) {
				switch_channel_event_set_data(channel, event);
				switch_event_fire(&event);
			}

			switch_core_session_rwunlock(psession);
			if (y) {
				stream->write_function(stream, "+OK\n");
				goto done;
			}
		} else {
			stream->write_function(stream, "-ERR No such channel!\n");
		}
	}

	stream->write_function(stream, "-USAGE: %s\n", UUID_MULTISET_SYNTAX);

  done:
	switch_safe_free(mycmd);
	return SWITCH_STATUS_SUCCESS;
}

static size_t header_callback(char *buffer, size_t size, size_t nitems, void *userdata)
{
	switch_event_t* event = (switch_event_t*)userdata;
	int len = strlen(buffer);
	char buf[1024];
	if(len > 2 && len < 1024) {
		strncpy(buf, buffer, len-2);
		buf[len-2] = '\0';
		switch_event_add_header_string(event, SWITCH_STACK_PUSH | SWITCH_STACK_BOTTOM, "Reply-Headers", buf);
	}
	return nitems * size;
}

SWITCH_STANDARD_API(kz_http_put)
{
	switch_status_t status = SWITCH_STATUS_SUCCESS;
	switch_memory_pool_t *lpool = NULL;
	switch_memory_pool_t *pool = NULL;
	char *args = NULL;
	char *argv[10] = { 0 };
	int argc = 0;
	switch_event_t *params = NULL;
	char *url = NULL;
	char *filename = NULL;

	switch_curl_slist_t *headers = NULL;  /* optional linked-list of HTTP headers */
	char *ext;  /* file extension, used for MIME type identification */
	const char *mime_type = "application/octet-stream";
	char *buf = NULL;
	char *error = NULL;

	CURL *curl_handle = NULL;
	long httpRes = 0;
	struct stat file_info = {0};
	FILE *file_to_put = NULL;
	int fd;

	if (session) {
		pool = switch_core_session_get_pool(session);
	} else {
		switch_core_new_memory_pool(&lpool);
		pool = lpool;
	}

	if (zstr(cmd)) {
		stream->write_function(stream, "USAGE: %s\n", KZ_HTTP_PUT_SYNTAX);
		status = SWITCH_STATUS_SUCCESS;
		goto done;
	}

	args = strdup(cmd);
	argc = switch_separate_string(args, ' ', argv, (sizeof(argv) / sizeof(argv[0])));
	if (argc != 2) {
		stream->write_function(stream, "USAGE: %s\n", KZ_HTTP_PUT_SYNTAX);
		status = SWITCH_STATUS_SUCCESS;
		goto done;
	}

	/* parse params and get profile */
	url = switch_core_strdup(pool, argv[0]);
	if (*url == '{') {
		switch_event_create_brackets(url, '{', '}', ',', &params, &url, SWITCH_FALSE);
	}

	filename = switch_core_strdup(pool, argv[1]);

	/* guess what type of mime content this is going to be */
	if ((ext = strrchr(filename, '.'))) {
		ext++;
		if (!(mime_type = switch_core_mime_ext2type(ext))) {
			mime_type = "application/octet-stream";
		}
	}

	buf = switch_mprintf("Content-Type: %s", mime_type);

	headers = switch_curl_slist_append(headers, buf);

	/* open file and get the file size */
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "opening %s for upload to %s\n", filename, url);
	fd = open(filename, O_RDONLY);
	if (fd == -1) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "open() error: %s\n", strerror(errno));
		status = SWITCH_STATUS_FALSE;
		stream->write_function(stream, "-ERR error opening file\n");
		goto done;
	}
	if (fstat(fd, &file_info) == -1) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "fstat() error: %s\n", strerror(errno));
		stream->write_function(stream, "-ERR fstat error\n");
		goto done;
	}
	close(fd);

	/* libcurl requires FILE* */
 	file_to_put = fopen(filename, "rb");
	if (!file_to_put) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "fopen() error: %s\n", strerror(errno));
		status = SWITCH_STATUS_FALSE;
		goto done;
	}

	curl_handle = switch_curl_easy_init();
	if (!curl_handle) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "switch_curl_easy_init() failure\n");
		status = SWITCH_STATUS_FALSE;
		stream->write_function(stream, "-ERR switch_curl_easy init failure\n");
		goto done;
	}
	switch_curl_easy_setopt(curl_handle, CURLOPT_UPLOAD, 1);
	switch_curl_easy_setopt(curl_handle, CURLOPT_PUT, 1);
	switch_curl_easy_setopt(curl_handle, CURLOPT_NOSIGNAL, 1);
	switch_curl_easy_setopt(curl_handle, CURLOPT_HTTPHEADER, headers);
	switch_curl_easy_setopt(curl_handle, CURLOPT_URL, url);
	switch_curl_easy_setopt(curl_handle, CURLOPT_READDATA, file_to_put);
	switch_curl_easy_setopt(curl_handle, CURLOPT_INFILESIZE_LARGE, (curl_off_t)file_info.st_size);
	switch_curl_easy_setopt(curl_handle, CURLOPT_FOLLOWLOCATION, 1);
	switch_curl_easy_setopt(curl_handle, CURLOPT_MAXREDIRS, 10);
	switch_curl_easy_setopt(curl_handle, CURLOPT_USERAGENT, "freeswitch-http-cache/1.0");
	switch_curl_easy_setopt(curl_handle, CURLOPT_HEADERDATA, stream->param_event);
	switch_curl_easy_setopt(curl_handle, CURLOPT_HEADERFUNCTION, header_callback);
	switch_curl_easy_setopt(curl_handle, CURLOPT_SSL_VERIFYPEER, 0L);
	switch_curl_easy_perform(curl_handle);
	switch_curl_easy_getinfo(curl_handle, CURLINFO_RESPONSE_CODE, &httpRes);
	switch_curl_easy_cleanup(curl_handle);

	if (httpRes == 200 || httpRes == 201 || httpRes == 202 || httpRes == 204) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "%s saved to %s\n", filename, url);
		switch_event_add_header(stream->param_event, SWITCH_STACK_BOTTOM, "API-Output", "%s saved to %s\n", filename, url);
		stream->write_function(stream, "+OK\n");
	} else {
		error = switch_mprintf("Received HTTP error %ld trying to save %s to %s", httpRes, filename, url);
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "%s\n", error);
		switch_event_add_header(stream->param_event, SWITCH_STACK_BOTTOM, "API-Error", "%s", error);
		switch_event_add_header(stream->param_event, SWITCH_STACK_BOTTOM, "API-HTTP-Error", "%ld", httpRes);
		stream->write_function(stream, "-ERR ");
		stream->write_function(stream, error);
		stream->write_function(stream, "\n");
		status = SWITCH_STATUS_GENERR;
	}

done:
	if (file_to_put) {
		fclose(file_to_put);
	}

	if (headers) {
		switch_curl_slist_free_all(headers);
	}

	switch_safe_free(buf);
	switch_safe_free(error);

	switch_safe_free(args);

	if (lpool) {
		switch_core_destroy_memory_pool(&lpool);
	}

	if (params) {
		switch_event_destroy(&params);
	}

	return status;
}

void add_kz_commands(switch_loadable_module_interface_t **module_interface, switch_api_interface_t *api_interface) {
	SWITCH_ADD_API(api_interface, "kz_uuid_setvar_multi", UUID_SET_DESC, uuid_setvar_multi_function, UUID_MULTISET_SYNTAX);
	switch_console_set_complete("add kz_uuid_setvar_multi ::console::list_uuid");
	SWITCH_ADD_API(api_interface, "kz_uuid_setvar", UUID_MULTISET_DESC, uuid_setvar_function, UUID_SET_SYNTAX);
	switch_console_set_complete("add kz_uuid_setvar ::console::list_uuid");
	SWITCH_ADD_API(api_interface, "kz_http_put", KZ_HTTP_PUT_DESC, kz_http_put, KZ_HTTP_PUT_SYNTAX);
}

