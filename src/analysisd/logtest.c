/* Copyright (C) 2015-2020, Wazuh Inc.
 * All right reserved.
 *
 * This program is free software; you can redistribute it
 * and/or modify it under the terms of the GNU General Public
 * License (version 2) as published by the FSF - Free Software
 * Foundation
 */

#include "logtest.h"


void *w_logtest_init() {

    w_logtest_connection_t connection;
    pthread_t * logtest_threads = NULL;

    if (w_logtest_init_parameters() == OS_INVALID) {
        merror(LOGTEST_ERROR_INV_CONF);
        return NULL;
    }

    if (!w_logtest_conf.enabled) {
        minfo(LOGTEST_DISABLED);
        return NULL;
    }

    if (connection.sock = OS_BindUnixDomain(LOGTEST_SOCK, SOCK_STREAM, OS_MAXSTR), connection.sock < 0) {
        merror(LOGTEST_ERROR_BIND_SOCK, LOGTEST_SOCK, errno, strerror(errno));
        return NULL;
    }

    if (w_logtest_sessions = OSHash_Create(), !w_logtest_sessions) {
        merror(LOGTEST_ERROR_INIT_HASH);
        return NULL;
    }

    if (!OSHash_setSize(w_logtest_sessions, w_logtest_conf.max_sessions*2)) {
        merror(LOGTEST_ERROR_SIZE_HASH);
        return NULL;
    }

    w_mutex_init(&connection.mutex, NULL);
    w_mutex_init(&connection.mutex_hash_table, NULL);
    connection.active_client = 0;

    minfo(LOGTEST_INITIALIZED);

    int num_extra_threads = w_logtest_conf.threads - 1;

    if (num_extra_threads > 0) {
        os_calloc(num_extra_threads, sizeof(pthread_t), logtest_threads);

        for (int i = 0; i < num_extra_threads; i++) {
            if (CreateThreadJoinable(logtest_threads + i, w_logtest_clients_handler, &connection)) {
                merror_exit(THREAD_ERROR);
            }
        }
    }

    w_create_thread(w_logtest_check_inactive_sessions, &connection);
    w_logtest_clients_handler(&connection);

    for (int i = 0; i < num_extra_threads; i++) {
        pthread_join(logtest_threads[i], NULL);
    }

    os_free(logtest_threads)

    close(connection.sock);
    if (unlink(LOGTEST_SOCK)) {
        merror(DELETE_ERROR, LOGTEST_SOCK, errno, strerror(errno));
    }

    w_mutex_destroy(&connection.mutex);
    w_mutex_destroy(&connection.mutex_hash_table);

    return NULL;
}


int w_logtest_init_parameters() {

    int modules = CLOGTEST;

    w_logtest_conf.enabled = true;
    w_logtest_conf.threads = LOGTEST_THREAD;
    w_logtest_conf.max_sessions = LOGTEST_MAX_SESSIONS;
    w_logtest_conf.session_timeout = LOGTEST_SESSION_TIMEOUT;

    if (ReadConfig(modules, OSSECCONF, NULL, NULL) < 0) {
        return OS_INVALID;
    }

    return OS_SUCCESS;
}


void * w_logtest_clients_handler(w_logtest_connection_t * connection) {

    int client;
    char msg_received[OS_MAXSTR];
    int size_msg_received;
    char * str_response;

    while (FOREVER()) {

        str_response = NULL;

        /* Wait for client */
        w_mutex_lock(&connection->mutex);
        if (client = accept(connection->sock, (struct sockaddr *) NULL, NULL), client < 0) {
            int err_accept = errno;
            w_mutex_unlock(&connection->mutex);
            merror(LOGTEST_ERROR_ACCEPT_CONN, strerror(err_accept));

            /* check if socket is closed */
            if (err_accept == EBADF) {
                return NULL;
            }
            continue;
        }
        w_mutex_unlock(&connection->mutex);

        switch (size_msg_received = OS_RecvSecureTCP(client, msg_received, OS_MAXSTR-1), size_msg_received) {
        case -1:
            mdebug1(LOGTEST_ERROR_RECV_MSG_ERRNO, strerror(errno));
            break;

        case 0:
            mdebug1(LOGTEST_ERROR_RECV_MSG_EMPTY_TO);
            break;

        case OS_SOCKTERR:
            mdebug1(LOGTEST_ERROR_RECV_MSG_OVERSIZE);
            if (str_response = w_logtest_generate_error_response(LOGTEST_ERROR_RECV_MSG_OVERSIZE), str_response) {
                OS_SendSecureTCP(client, strlen(str_response), str_response);
            }
            break;

        default:
            if (str_response = w_logtest_process_request(msg_received, connection), str_response) {
                OS_SendSecureTCP(client, strlen(str_response), str_response);
            }
        }

        os_free(str_response);
        close(client);
    }

    return NULL;
}


char *w_logtest_generate_error_response(char * msg){

    cJSON * json_response = NULL;
    cJSON * json_msg_list = NULL;
    cJSON * json_msg = NULL;
    char * str_response = NULL;

    json_response =  cJSON_CreateObject();
    json_msg_list = cJSON_CreateArray();
    json_msg = cJSON_CreateString(msg);

    cJSON_AddItemToArray(json_msg_list, json_msg);
    cJSON_AddItemToObject(json_response,W_LOGTEST_JSON_MESSAGES, json_msg_list);
    cJSON_AddNumberToObject(json_response, W_LOGTEST_JSON_CODE, W_LOGTEST_RCODE_ERROR_INPUT);

    str_response = cJSON_PrintUnformatted(json_response);

    cJSON_Delete(json_response);

    return str_response;
}


cJSON *w_logtest_process_log(cJSON * request, w_logtest_session_t * session, bool * alert_generated, OSList * list_msg) {

    cJSON *output = NULL;
    Eventinfo *lf = NULL;
    int check_add_event;

    /* Initialize eventinfo which will contain alert information */
    os_calloc(1, sizeof(Eventinfo), lf);
    os_calloc(Config.decoder_order_size, sizeof(DynamicField), lf->fields);
    Zero_Eventinfo(lf);

    /* Preprocessing */
    if (w_logtest_preprocessing_phase(lf, request) != 0) {
        Free_Eventinfo(lf);
        smerror(list_msg, FORMAT_ERROR);
        return output;
    }

    /* Decoding */
    w_logtest_decoding_phase(lf, session);

    /* Run accumulator */
    if (lf->decoder_info->accumulate == 1) {
        lf = Accumulate(lf, &session->acm_store, &session->acm_lookups, &session->acm_purge_ts);
    }

    /* Rules matching */
    if (check_add_event = w_logtest_rulesmatching_phase(lf, session, list_msg), check_add_event == -1) {
        Free_Eventinfo(lf);
        return output;
    }

    /* Add alert description to the event and check alert level if exist a match */
    if (lf->generated_rule) {
        lf->comment = ParseRuleComment(lf);
        *alert_generated = ((check_add_event == 1) && Config.logbylevel <= lf->generated_rule->level) ? true : false;
    }

    /* Parse the alert */
    char *output_str = Eventinfo_to_jsonstr(lf, false);
    output = cJSON_Parse(output_str);
    os_free(output_str);

    /* Clear the memory if the event was not added to the stateful memory */
    if (check_add_event == 0) {
        Free_Eventinfo(lf);
    }

    return output;
}


int w_logtest_preprocessing_phase(Eventinfo * lf, cJSON * request) {

    char * event_str = NULL;
    char * location_str = NULL;
    char * log = NULL;
    cJSON * event = NULL;
    cJSON * location = NULL;
    bool event_json = false;

    event = cJSON_GetObjectItemCaseSensitive(request, W_LOGTEST_JSON_EVENT);

    if (event->child) {
        event_json = true;
        event_str = cJSON_PrintUnformatted(event);
    }
    else {
        event_str = cJSON_GetStringValue(event);
    }

    location = cJSON_GetObjectItemCaseSensitive(request, W_LOGTEST_JSON_LOCATION);
    location_str = cJSON_GetStringValue(location);

    int logsize = strlen(location_str) + strlen(event_str) + 4;

    os_calloc(logsize, sizeof(char), log);
    snprintf(log, logsize, "1:%s:%s", location_str, event_str);

    if (OS_CleanMSG(log, lf) < 0) {
        os_free(log);
        if (event_json) os_free(event_str);
        return -1;
    }

    lf->size = strlen(lf->log);

    os_free(log);
    if (event_json) os_free(event_str);

    return 0;
}


void w_logtest_decoding_phase(Eventinfo * lf, w_logtest_session_t * session) {

    OSDecoderNode * decodernode = NULL;

    if (lf->program_name) {
        decodernode = session->decoderlist_forpname;
    } else {
        decodernode = session->decoderlist_nopname;
    }

    DecodeEvent(lf, Config.g_rules_hash, &session->decoder_match, decodernode);
}


int w_logtest_rulesmatching_phase(Eventinfo * lf, w_logtest_session_t * session, OSList * list_msg) {

    RuleNode * rulenode = NULL;
    RuleInfo * ruleinformation = NULL;
    bool added_list_event = false;

    if (rulenode = session->rule_list, !rulenode) {
        return -1;
    }

    do {

        if (lf->decoder_info->type == OSSEC_ALERT && !lf->generated_rule) {
            break;
        }

        /* The categories must match */
        if (rulenode->ruleinfo->category != lf->decoder_info->type) {
            continue;
        }

        /* Search the rule that match */
        if (ruleinformation = OS_CheckIfRuleMatch(lf, session->eventlist, &session->cdblistnode,
            rulenode, &session->rule_match, &session->fts_list, &session->fts_store), !ruleinformation) {
            continue;
        }

        lf->generated_rule = ruleinformation;

        /* Ignore level 0 */
        if (ruleinformation->level == 0) {
            break;
        }

        /* Check ignore time */
        if (ruleinformation->ignore_time) {

            if (ruleinformation->time_ignored == 0) {
                ruleinformation->time_ignored = lf->generate_time;
            } else if ((lf->generate_time - ruleinformation->time_ignored) < ruleinformation->ignore_time) {
                /* If the current time - the time the rule was ignored is less than the time it should be ignored,
                   do not alert again */
                break;
            } else {
                ruleinformation->time_ignored = 0;
            }
        }

        /* Check if we should ignore it */
        if (ruleinformation->ckignore && IGnore(lf, 0)) {
            break;
        }


        /* Copy the structure to the state memory of if_matched_sid */
        if (ruleinformation->sid_prev_matched) {

            if (!OSList_AddData(ruleinformation->sid_prev_matched, lf)) {
                smerror(list_msg, "Unable to add data to sig list.");
            } else {
                lf->sid_node_to_delete = ruleinformation->sid_prev_matched->last_node;
            }
        }

        /* Group list */
        else if (ruleinformation->group_prev_matched) {
            OSListNode *node;
            os_calloc(ruleinformation->group_prev_matched_sz, sizeof(OSListNode *), lf->group_node_to_delete);
            for (unsigned int i = 0; i < ruleinformation->group_prev_matched_sz; i++) {
                if (node = OSList_AddData(ruleinformation->group_prev_matched[i], lf), node) {
                    lf->group_node_to_delete[i] = node;
                } else {
                    smerror(list_msg, "Unable to add data to grp list.");
                }
            }
        }

        OS_AddEvent(lf, session->eventlist);
        added_list_event = true;
        break;

    } while(rulenode = rulenode->next, rulenode);

    return added_list_event ? 1 : 0;
}


w_logtest_session_t *w_logtest_initialize_session(char *token, OSList* list_msg) {

    w_logtest_session_t * session;
    bool retval = true;

    char **files;

    os_calloc(1, sizeof(w_logtest_session_t), session);

    session->token = token;
    session->expired = false;
    session->last_connection = time(NULL);
    w_mutex_init(&session->mutex, NULL);

    /* Create list to save previous events */
    os_calloc(1, sizeof(EventList), session->eventlist);
    OS_CreateEventList(Config.memorysize, session->eventlist);

    /* Load decoders */
    session->decoderlist_forpname = NULL;
    session->decoderlist_nopname = NULL;
    session->decoder_store = NULL;

    files = Config.decoders;

    while (files && *files) {
        if (!ReadDecodeXML(*files, &session->decoderlist_forpname,
            &session->decoderlist_nopname, &session->decoder_store, list_msg)) {
            goto cleanup;
        }
        files++;
    }

    SetDecodeXML(list_msg, &session->decoder_store, &session->decoderlist_nopname, &session->decoderlist_forpname);

    /* Load CDB list */
    session->cdblistnode = NULL;
    session->cdblistrule = NULL;

    files = Config.lists;

    while (files && *files) {
        if (Lists_OP_LoadList(*files, &session->cdblistnode, list_msg) < 0) {
            goto cleanup;
        }
        files++;
    }

    Lists_OP_MakeAll(0, 0, &session->cdblistnode);

    /* Load rules */
    session->rule_list = NULL;

    files = Config.includes;

    while (files && *files) {
        if (Rules_OP_ReadRules(*files, &session->rule_list, &session->cdblistnode,
                            &session->eventlist, &session->decoder_store, list_msg) < 0) {
            goto cleanup;
        }
        files++;
    }

    /* Associate rules and CDB lists */
    OS_ListLoadRules(&session->cdblistnode, &session->cdblistrule);

    /* _setlevels */
    _setlevels(session->rule_list, 0);

    /* Creating rule hash */
    if (session->g_rules_hash = OSHash_Create(), !session->g_rules_hash) {
        goto cleanup;
    }

    AddHash_Rule(session->rule_list);

    /* Initiate the FTS list */
    if (!w_logtest_fts_init(&session->fts_list, &session->fts_store)) {
        goto cleanup;
    }

    /* Initialize the Accumulator */
    if (!Accumulate_Init(&session->acm_store, &session->acm_lookups, &session->acm_purge_ts)) {
        goto cleanup;
    }

    /* Set rule_match and decoder_match to zero */
    memset(&session->decoder_match, 0, sizeof(regex_matching));
    memset(&session->rule_match, 0, sizeof(regex_matching));

    retval = false;

cleanup:

    if (retval) {

        /* Remove list of previous events */
        os_remove_eventlist(session->eventlist);

        /* Remove rule list and rule hash */
        os_remove_rules_list(session->rule_list);
        if (session->g_rules_hash) {
            OSHash_Free(session->g_rules_hash);
        }

        /* Remove decoder lists */
        os_remove_decoders_list(session->decoderlist_forpname, session->decoderlist_nopname);
        OSStore_Free(session->decoder_store);

        /* Remove cdblistnode and cdblistrule */
        os_remove_cdblist(&session->cdblistnode);
        os_remove_cdbrules(&session->cdblistrule);

        /* Remove fts list and hash */
        if (session->fts_store) {
            OSHash_Free(session->fts_store);
        }
        os_free(session->fts_list);

        /* Remove accumulator hash */
        if (session->acm_store) {
            OSHash_Free(session->acm_store);
        }

        /* Remove session */
        w_mutex_destroy(&session->mutex);

        /* Free memory allocated in OSRegex execution */
        OSRegex_free_regex_matching(&session->decoder_match);
        OSRegex_free_regex_matching(&session->rule_match);

        /* Remove session */
        os_free(session);
    }


    return session;
}

void w_logtest_remove_session(char *token) {

    w_logtest_session_t *session;

    /* Remove session from hash */
    if (session = OSHash_Delete_ex(w_logtest_sessions, token), !session) {
        return;
    }

    /* Remove list of previous events */
    os_remove_eventlist(session->eventlist);

    /* Remove rule list and rule hash */
    os_remove_rules_list(session->rule_list);
    OSHash_Free(session->g_rules_hash);

    /* Remove decoder lists */
    os_remove_decoders_list(session->decoderlist_forpname, session->decoderlist_nopname);
    OSStore_Free(session->decoder_store);

    /* Remove cdblistnode and cdblistrule */
    os_remove_cdblist(&session->cdblistnode);
    os_remove_cdbrules(&session->cdblistrule);

    /* Remove fts list and hash */
    OSHash_Free(session->fts_store);
    os_free(session->fts_list);

    /* Remove accumulator hash */
    OSHash_Free(session->acm_store);

    /* Remove session */
    w_mutex_destroy(&session->mutex);

    /* Free memory allocated in OSRegex execution */
    OSRegex_free_regex_matching(&session->decoder_match);
    OSRegex_free_regex_matching(&session->rule_match);

    /* Remove token and session */
    os_free(session->token);
    os_free(session);
}


void *w_logtest_check_inactive_sessions(w_logtest_connection_t * connection) {

    OSHashNode *hash_node;
    unsigned int inode_it = 0;
    time_t current_time;

    while (FOREVER()) {

        sleep(w_logtest_conf.session_timeout);
        w_mutex_lock(&connection->mutex_hash_table);

        hash_node = OSHash_Begin(w_logtest_sessions, &inode_it);

        while (hash_node) {
            char *token_session;
            w_logtest_session_t *session = NULL;

            token_session = hash_node->key;
            session = hash_node->data;

            w_mutex_lock(&session->mutex);
            current_time = time(NULL);
            if (difftime(current_time, session->last_connection) >= w_logtest_conf.session_timeout) {
                session->expired = true;
            }

            w_mutex_unlock(&session->mutex);

            hash_node = OSHash_Next(w_logtest_sessions, &inode_it, hash_node);

            if (session->expired) {
                connection->active_client -= 1;
                w_logtest_remove_session(token_session);
            }

        }

        w_mutex_unlock(&connection->mutex_hash_table);

    }

    return NULL;

}


int w_logtest_fts_init(OSList **fts_list, OSHash **fts_store) {

    int list_size = getDefine_Int("analysisd", "fts_list_size", 12, 512);

    if (*fts_list = OSList_Create(), *fts_list == NULL) {
        merror(LIST_ERROR);
        return 0;
    }

    if (!OSList_SetMaxSize(*fts_list, list_size)) {
        merror(LIST_SIZE_ERROR);
        return 0;
    }

    if (*fts_store = OSHash_Create(), *fts_store == NULL) {
        merror(HASH_ERROR);
        return 0;
    }

    if (!OSHash_setSize(*fts_store, 2048)) {
        merror(LIST_SIZE_ERROR);
        return 0;
    }

    return 1;
}


int w_logtest_check_input(char * input_json, cJSON ** req, OSList * list_msg) {

    /* Node JSON input */
    cJSON * root;

    int retval = W_LOGTEST_REQUEST_ERROR;

    /* Parse raw JSON input */
    const char * jsonErrPtr;
    root = cJSON_ParseWithOpts(input_json, &jsonErrPtr, 0);
    *req = root;
    if (!root) {
        char * slice_json;
        char * pos;

        os_calloc(W_LOGTEST_ERROR_JSON_PARSE_NSTR + 1, sizeof(char), slice_json);
        pos = (char *) (jsonErrPtr - W_LOGTEST_ERROR_JSON_PARSE_NSTR / 2 < input_json
                            ? input_json
                            : jsonErrPtr - W_LOGTEST_ERROR_JSON_PARSE_NSTR / 2);

        snprintf(slice_json, W_LOGTEST_ERROR_JSON_PARSE_NSTR + 1, "%s", pos);

        mdebug1(LOGTEST_ERROR_JSON_PARSE);
        smerror(list_msg, LOGTEST_ERROR_JSON_PARSE);

        mdebug1(LOGTEST_ERROR_JSON_PARSE_POS, (int) (jsonErrPtr - input_json), slice_json);
        smerror(list_msg, LOGTEST_ERROR_JSON_PARSE_POS, (int) (jsonErrPtr - input_json), slice_json);

        os_free(slice_json);
        return retval;
    }

    if (cJSON_GetObjectItemCaseSensitive(root, W_LOGTEST_JSON_REMOVE_SESSION)) {
        retval = w_logtest_check_input_remove_session(root, list_msg);
    } else {
        retval = w_logtest_check_input_request(root, list_msg);
    }


    return retval;
}

int w_logtest_check_input_remove_session(cJSON * root, OSList * list_msg) {

    cJSON * token;

    int retval = W_LOGTEST_REQUEST_TYPE_REMOVE_SESSION;

    token = cJSON_GetObjectItemCaseSensitive(root, W_LOGTEST_JSON_REMOVE_SESSION);
    if (!cJSON_IsString(token) || (cJSON_IsString(token) && token->valuestring == NULL) ) {

        mdebug1(LOGTEST_ERROR_TOKEN_INVALID_TYPE);
        smerror(list_msg, LOGTEST_ERROR_TOKEN_INVALID_TYPE);

        retval = W_LOGTEST_REQUEST_ERROR;

    } else if (cJSON_IsString(token) && token->valuestring != NULL
               && strlen(token->valuestring) != W_LOGTEST_TOKEN_LENGH) {

        mdebug1(LOGTEST_ERROR_TOKEN_INVALID, token->valuestring);
        smerror(list_msg, LOGTEST_ERROR_TOKEN_INVALID, token->valuestring);

        retval = W_LOGTEST_REQUEST_ERROR;
    }

    return retval;

}

int w_logtest_check_input_request(cJSON * root, OSList * list_msg) {

    cJSON * location;
    cJSON * log_format;
    cJSON * event;
    cJSON * token;

    int retval = W_LOGTEST_REQUEST_TYPE_LOG_PROCESSING;

    /* Check JSON fields */
    location = cJSON_GetObjectItemCaseSensitive(root, W_LOGTEST_JSON_LOCATION);
    if (!(cJSON_IsString(location) && location->valuestring != NULL && strlen(location->valuestring) > 0)) {

        mdebug1(LOGTEST_ERROR_JSON_REQUIRED_SFIELD, W_LOGTEST_JSON_LOCATION);
        smerror(list_msg, LOGTEST_ERROR_JSON_REQUIRED_SFIELD, W_LOGTEST_JSON_LOCATION);
        retval = W_LOGTEST_REQUEST_ERROR;
    }

    log_format = cJSON_GetObjectItemCaseSensitive(root, W_LOGTEST_JSON_LOGFORMAT);
    if (!(cJSON_IsString(log_format) && log_format->valuestring != NULL
          && strlen(log_format->valuestring) > 0)) {

        mdebug1(LOGTEST_ERROR_JSON_REQUIRED_SFIELD, W_LOGTEST_JSON_LOGFORMAT);
        smerror(list_msg, LOGTEST_ERROR_JSON_REQUIRED_SFIELD, W_LOGTEST_JSON_LOGFORMAT);
        retval = W_LOGTEST_REQUEST_ERROR;
    }

    /* An event can be a string or a json object */
    event = cJSON_GetObjectItemCaseSensitive(root, W_LOGTEST_JSON_EVENT);
    if ( !(cJSON_IsString(event) && event->valuestring != NULL && strlen(event->valuestring) > 0)
         && !(cJSON_IsObject(event) && event->child != NULL)) {

        mdebug1(LOGTEST_ERROR_FIELD_NOT_FOUND, W_LOGTEST_JSON_EVENT);
        smerror(list_msg, LOGTEST_ERROR_FIELD_NOT_FOUND, W_LOGTEST_JSON_EVENT);
        retval = W_LOGTEST_REQUEST_ERROR;
    }

    token = cJSON_GetObjectItemCaseSensitive(root, W_LOGTEST_JSON_TOKEN);
    if (token && (!cJSON_IsString(token) || !valid_str_session(token, W_LOGTEST_TOKEN_LENGH)) {
        
        char * str_token = NULL;
        
        if (cJSON_IsString(token)) {
            os_strdup(token->valuestring, str_token);
        } else {
            str_token = cJSON_PrintUnformatted(token);
        }

        mdebug1(LOGTEST_ERROR_TOKEN_INVALID, str_token);
        smwarn(list_msg, LOGTEST_ERROR_TOKEN_INVALID, str_token);
        os_free(str_token);

        cJSON_DeleteItemFromObjectCaseSensitive(root, W_LOGTEST_JSON_TOKEN);
    }

    return retval;
}


w_logtest_session_t * w_logtest_get_session(cJSON * req, OSList * list_msg, w_logtest_connection_t * connection) {

    w_logtest_session_t * session = NULL;

    cJSON * j_token;
    char * s_token = NULL;

    if (j_token = cJSON_GetObjectItemCaseSensitive(req, W_LOGTEST_JSON_TOKEN), j_token) {
        s_token = j_token->valuestring;
    }

    /* Search an active session */
    if (s_token) {
        if (session = OSHash_Get_ex(w_logtest_sessions, s_token), session) {

            w_mutex_lock(&session->mutex);
            if (!session->expired) {
                session->last_connection = time(NULL);
                w_mutex_unlock(&session->mutex);
                return session;
            }
            w_mutex_unlock(&session->mutex);
        }

        mdebug1(LOGTEST_WARN_TOKEN_EXPIRED, s_token);
        smwarn(list_msg, LOGTEST_WARN_TOKEN_EXPIRED, s_token);
    }

    /* New session */
    s_token = NULL;
    do {
        os_free(s_token);
        s_token = w_logtest_generate_token();
    } while (OSHash_Get_ex(w_logtest_sessions, s_token) != NULL);

    session = w_logtest_initialize_session(s_token, list_msg);
    if (session) {
        w_logtest_register_session(connection, session);
        mdebug1(LOGTEST_INFO_TOKEN_SESSION, s_token);
        sminfo(list_msg, LOGTEST_INFO_TOKEN_SESSION, s_token);
    } else {
        smerror(list_msg, LOGTEST_ERROR_INITIALIZE_SESSION, s_token);
        mdebug1(LOGTEST_ERROR_INITIALIZE_SESSION, s_token);
        os_free(s_token);
    }

    return session;
}


void w_logtest_register_session(w_logtest_connection_t * connection, w_logtest_session_t * session) {

    w_mutex_lock(&connection->mutex_hash_table);
    connection->active_client += 1;

    /* Find the client who has not made a query for the longest time and remove session */
    if (connection->active_client > w_logtest_conf.max_sessions) {
        w_logtest_remove_old_session(connection);
    }

    w_mutex_unlock(&connection->mutex_hash_table);

    /* Register session */
    OSHash_Add_ex(w_logtest_sessions, session->token, session);
}

void w_logtest_remove_old_session(w_logtest_connection_t * connection) {

        OSHashNode * hash_node;
        unsigned int inode_it = 0;

        w_logtest_session_t * current_session = NULL;
        w_logtest_session_t * old_session = NULL;
        bool exchange = false;

        hash_node = OSHash_Begin(w_logtest_sessions, &inode_it);
        old_session = hash_node->data;
        hash_node = OSHash_Next(w_logtest_sessions, &inode_it, hash_node);

        while (hash_node) {

            current_session = hash_node->data;

            w_mutex_lock(&current_session->mutex);
            w_mutex_lock(&old_session->mutex);

            exchange = old_session->last_connection > current_session->last_connection;

            w_mutex_unlock(&old_session->mutex);
            w_mutex_unlock(&current_session->mutex);

            if (exchange) {
                old_session = current_session;
            }

            hash_node = OSHash_Next(w_logtest_sessions, &inode_it, hash_node);
        }

        /* Remove old session */
        connection->active_client -= 1;
        w_logtest_remove_session(old_session->token);
}

char * w_logtest_generate_token() {

    char * str_token;
    int32_t int_token;

    os_calloc(W_LOGTEST_TOKEN_LENGH + 1, sizeof(char), str_token);
    randombytes((void *) &int_token, sizeof(int32_t));
    snprintf(str_token, W_LOGTEST_TOKEN_LENGH + 1, "%08x", int_token);

    return str_token;
}


void w_logtest_add_msg_response(cJSON * response, OSList * list_msg, int * error_code) {

    int ret_level = *error_code;

    OSListNode * node_log_msg;
    cJSON * json_arr_msg;

    if (node_log_msg = OSList_GetFirstNode(list_msg), !node_log_msg) {
        return;
    }
    /* check if exist messages in the response */
    else if (json_arr_msg = cJSON_GetObjectItemCaseSensitive(response, W_LOGTEST_JSON_MESSAGES), !json_arr_msg) {
        json_arr_msg = cJSON_CreateArray();
        cJSON_AddItemToObject(response, W_LOGTEST_JSON_MESSAGES, json_arr_msg);
    }

    while (node_log_msg) {
        cJSON * json_item_message;
        char * json_str_msj = NULL;
        char * raw_msg;

        /* get msg */
        os_analysisd_log_msg_t * data_msg = node_log_msg->data;
        raw_msg = os_analysisd_string_log_msg(data_msg);

        /* Add header and set max level of msgs */
        if (data_msg->level == LOGLEVEL_ERROR) {

            ret_level = W_LOGTEST_RCODE_ERROR_PROCESS;
            wm_strcat(&json_str_msj, "ERROR: ", 0);

        } else if (data_msg->level == LOGLEVEL_WARNING) {

            if (ret_level != W_LOGTEST_RCODE_ERROR_PROCESS) {
                ret_level = W_LOGTEST_RCODE_WARNING;
            }
            wm_strcat(&json_str_msj, "WARNING: ", 0);

        } else {
            wm_strcat(&json_str_msj, "INFO: ", 0);
        }

        /* Add to json array */
        wm_strcat(&json_str_msj, raw_msg, 0);
        json_item_message = cJSON_CreateString(json_str_msj);
        cJSON_AddItemToArray(json_arr_msg, json_item_message);

        /* Cleanup */
        os_free(raw_msg);
        os_free(json_str_msj);
        os_analysisd_free_log_msg(&data_msg);
        OSList_DeleteCurrentlyNode(list_msg);

        node_log_msg = OSList_GetFirstNode(list_msg);
    }

    *error_code = ret_level;
    return;
}


char * w_logtest_process_request(char * raw_request, w_logtest_connection_t * connection) {

    char * str_response = NULL;
    int request_type;
    cJSON * json_request = NULL;
    cJSON * json_response = NULL;

    /* error & message handlers */
    int retval = W_LOGTEST_RCODE_SUCCESS;
    OSList * list_msg = OSList_Create();
    if (!list_msg) {
        merror(LIST_ERROR);
        return NULL;
    }
    OSList_SetMaxSize(list_msg, ERRORLIST_MAXSIZE);

    /* Check message and generate a request */
    json_response = cJSON_CreateObject();
    request_type = w_logtest_check_input(raw_request, &json_request, list_msg);

    if (request_type == W_LOGTEST_REQUEST_ERROR) {

        w_logtest_add_msg_response(json_response, list_msg, &retval);
        retval = W_LOGTEST_RCODE_ERROR_INPUT;

    } else if (request_type == W_LOGTEST_REQUEST_TYPE_LOG_PROCESSING) {

        retval = w_logtest_process_request_log_processing(json_request, json_response, list_msg, connection);

    } else if (request_type == W_LOGTEST_REQUEST_TYPE_REMOVE_SESSION) {

        retval = w_logtest_process_request_remove_session(json_request, json_response, list_msg, connection);

    }

    cJSON_AddNumberToObject(json_response, W_LOGTEST_JSON_CODE, retval);

    str_response = cJSON_PrintUnformatted(json_response);
    cJSON_Delete(json_response);
    cJSON_Delete(json_request);
    os_free(list_msg);

    return str_response;
}

int w_logtest_process_request_log_processing(cJSON * json_request, cJSON * json_response, OSList * list_msg,
                                             w_logtest_connection_t * connection) {

    w_logtest_session_t * current_session = NULL;
    cJSON * json_log_processed = NULL;

    int retval = W_LOGTEST_RCODE_SUCCESS;
    bool alert_generated = false;

    /* Get session */
    current_session = w_logtest_get_session(json_request, list_msg, connection);
    if (current_session) {
        cJSON_AddStringToObject(json_response, W_LOGTEST_JSON_TOKEN, current_session->token);
    }
    w_logtest_add_msg_response(json_response, list_msg, &retval);

    /* Proccess log */
    if (retval >= W_LOGTEST_RCODE_SUCCESS && current_session) {

        json_log_processed = w_logtest_process_log(json_request, current_session, &alert_generated, list_msg);
        if (json_log_processed) {
            cJSON_AddItemToObject(json_response, W_LOGTEST_JSON_OUTPUT, json_log_processed);
        } else {
            smerror(list_msg, LOGTEST_ERROR_PROCESS_EVENT);
            mdebug1(LOGTEST_ERROR_PROCESS_EVENT);
        }
        w_logtest_add_msg_response(json_response, list_msg, &retval);
    }

    /* Check alert level */
    if (retval >= W_LOGTEST_RCODE_SUCCESS) {
        cJSON_AddBoolToObject(json_response, W_LOGTEST_JSON_ALERT, alert_generated ? 1 : 0);
    }

    return retval;
}

int w_logtest_process_request_remove_session(cJSON * json_request, cJSON * json_response, OSList * list_msg,
                                             w_logtest_connection_t * connection) {

    w_logtest_session_t * session = NULL;
    int retval = W_LOGTEST_RCODE_SUCCESS;
    cJSON * j_token = NULL;
    char * s_token = NULL;

    j_token = cJSON_GetObjectItemCaseSensitive(json_request, W_LOGTEST_JSON_REMOVE_SESSION);

    if (j_token && j_token->valuestring != NULL) {
        s_token = j_token->valuestring;

        w_mutex_lock(&connection->mutex_hash_table);

        if (session = OSHash_Get_ex(w_logtest_sessions, s_token), session) {

            connection->active_client -= 1;
            w_logtest_remove_session(s_token);
            sminfo(list_msg, LOGTEST_INFO_SESSION_REMOVE, s_token);

        } else {
            smerror(list_msg, LOGTEST_WARN_SESSION_NOT_FOUND, s_token);
            mdebug1(LOGTEST_WARN_SESSION_NOT_FOUND, s_token);
        }

        w_mutex_unlock(&connection->mutex_hash_table);

    } else {
        smerror(list_msg, LOGTEST_ERROR_TOKEN_INVALID_TYPE);
        mdebug1(LOGTEST_ERROR_TOKEN_INVALID_TYPE);
    }

    w_logtest_add_msg_response(json_response, list_msg, &retval);

    return retval;
}