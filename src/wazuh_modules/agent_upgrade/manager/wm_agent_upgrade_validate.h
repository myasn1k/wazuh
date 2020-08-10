/*
 * Wazuh Module for Agent Upgrading
 * Copyright (C) 2015-2020, Wazuh Inc.
 * August 10, 2020.
 *
 * This program is free software; you can redistribute it
 * and/or modify it under the terms of the GNU General Public
 * License (version 2) as published by the FSF - Free Software
 * Foundation.
 */
#ifndef WM_AGENT_UPGRADE_VALIDATE_H
#define WM_AGENT_UPGRADE_VALIDATE_H

#include "wm_agent_upgrade_manager.h"

/**
 * Check if agent exist
 * @param agent_id id of agent to validate
 * @return return_code
 * @retval WM_UPGRADE_SUCCESS
 * @retval WM_UPGRADE_INVALID_ACTION_FOR_MANAGER
 * */
int wm_agent_upgrade_validate_id(int agent_id);

/**
 * Check if agent status is active
 * @param keep_alive last keep-alive of agent to validate
 * @return return_code
 * @retval WM_UPGRADE_SUCCESS
 * @retval WM_UPGRADE_AGENT_IS_NOT_ACTIVE
 * */
int wm_agent_upgrade_validate_status(int last_keep_alive);

/**
 * Check if agent is valid to upgrade
 * @param agent_info pointer to agent_info struture
 * @param task pointer to task with the params
 * @param command wm_upgrade_command with the selected upgrade type
 * @return return_code
 * @retval WM_UPGRADE_SUCCESS
 * @retval WM_UPGRADE_NOT_MINIMAL_VERSION_SUPPORTED
 * @retval WM_UPGRADE_SYSTEM_NOT_SUPPORTED
 * @retval WM_UPGRADE_URL_NOT_FOUND
 * @retval WM_UPGRADE_WPK_VERSION_DOES_NOT_EXIST
 * @retval WM_UPGRADE_NEW_VERSION_LEES_OR_EQUAL_THAT_CURRENT
 * @retval WM_UPGRADE_NEW_VERSION_GREATER_MASTER
 * @retval WM_UPGRADE_VERSION_SAME_MANAGER
 * @retval WM_UPGRADE_GLOBAL_DB_FAILURE
 * */
int wm_agent_upgrade_validate_version(const wm_agent_info *agent_info, void *task, wm_upgrade_command command);

/**
 * Check if WPK file exist or download it
 * @param task pointer to task with the params
 * @return return_code
 * @retval WM_UPGRADE_SUCCESS
 * @retval WM_UPGRADE_WPK_FILE_DOES_NOT_EXIST
 * @retval WM_UPGRADE_WPK_SHA1_DOES_NOT_MATCH
 * */
int wm_agent_upgrade_validate_wpk(const wm_upgrade_task *task);

/**
 * Check if WPK custom file exist
 * @param task pointer to task with the params
 * @return return_code
 * @retval WM_UPGRADE_SUCCESS
 * @retval WM_UPGRADE_WPK_FILE_DOES_NOT_EXIST
 * */
int wm_agent_upgrade_validate_wpk_custom(const wm_upgrade_custom_task *task);

/**
 * Validate a status response from the task manager module
 * @param response JSON to be validated
 * @param status string to save the status of the task
 * Example format:
 * [{
 *      "error": 0,
 *      "data": "Success",
 *      "agent": 1,
 *      "status": "Done"
 *  }, {
 *      "error": 7,
 *      "data": "No task in DB",
 *      "agent": 2,
 *      "status": "Done"
 *  }]
 * */
bool wm_agent_upgrade_validate_task_status_message(const cJSON *response, char **status);

#endif
