﻿// Copyright (c) rAthena Dev Teams - Licensed under GNU GPL
// For more information, see LICENCE in the main folder

#include "achievement.hpp"

#include <array>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <yaml-cpp/yaml.h>

#include "../common/cbasetypes.hpp"
#include "../common/malloc.hpp"
#include "../common/nullpo.hpp"
#include "../common/showmsg.hpp"
#include "../common/strlib.hpp"
#include "../common/utils.hpp"

#include "battle.hpp"
#include "chrif.hpp"
#include "clif.hpp"
#include "intif.hpp"
#include "itemdb.hpp"
#include "map.hpp"
#include "npc.hpp"
#include "pc.hpp"
#include "script.hpp"
#include "status.hpp"

std::unordered_map<int, std::shared_ptr<s_achievement_db>> achievements;
std::vector<int> achievement_mobs; // Avoids checking achievements on every mob killed

/**
 * Searches an achievement by ID
 * @param achievement_id: ID to lookup
 * @return True if achievement exists or false if it doesn't
 */
bool achievement_exists(int achievement_id)
{
	return achievements.find(achievement_id) != achievements.end();
}

/**
 * Return an achievement by ID
 * @param achievement_id: ID to lookup
 * @return shared_ptr of achievement
 */
std::shared_ptr<s_achievement_db> &achievement_get(int achievement_id) 
{
	return achievements[achievement_id];
}

/**
 * Searches for an achievement by monster ID
 * @param mob_id: Monster ID to lookup
 * @return True on success, false on failure
 */
bool achievement_mobexists(int mob_id)
{
	if (!battle_config.feature_achievement)
		return false;

	auto it = std::find(achievement_mobs.begin(), achievement_mobs.end(), mob_id);

	return (it != achievement_mobs.end()) ? true : false;
}

/**
 * Add an achievement to the player's log
 * @param sd: Player data
 * @param achievement_id: Achievement to add
 * @return NULL on failure, achievement data on success
 */
struct achievement *achievement_add(struct map_session_data *sd, int achievement_id)
{
	int i, index;

	nullpo_retr(NULL, sd);

	if (!achievement_exists(achievement_id)) {
		ShowError("achievement_add: Achievement %d not found in DB.\n", achievement_id);
		return NULL;
	}

	auto &adb = achievements[achievement_id];

	ARR_FIND(0, sd->achievement_data.count, i, sd->achievement_data.achievements[i].achievement_id == achievement_id);
	if (i < sd->achievement_data.count) {
		ShowError("achievement_add: Character %d already has achievement %d.\n", sd->status.char_id, achievement_id);
		return NULL;
	}

	index = sd->achievement_data.incompleteCount;

	sd->achievement_data.count++;
	sd->achievement_data.incompleteCount++;
	RECREATE(sd->achievement_data.achievements, struct achievement, sd->achievement_data.count);

	// The character has some completed achievements, make room before them so that they will stay at the end of the array
	if (sd->achievement_data.incompleteCount != sd->achievement_data.count)
		memmove(&sd->achievement_data.achievements[index + 1], &sd->achievement_data.achievements[index], sizeof(struct achievement) * (sd->achievement_data.count - sd->achievement_data.incompleteCount));

	memset(&sd->achievement_data.achievements[index], 0, sizeof(struct achievement));

	sd->achievement_data.achievements[index].achievement_id = achievement_id;
	sd->achievement_data.achievements[index].score = adb->score;
	sd->achievement_data.save = true;

	clif_achievement_update(sd, &sd->achievement_data.achievements[index], sd->achievement_data.count - sd->achievement_data.incompleteCount);

	return &sd->achievement_data.achievements[index];
}

/**
 * Removes an achievement from a player's log
 * @param sd: Player's data
 * @param achievement_id: Achievement to remove
 * @return True on success, false on failure
 */
bool achievement_remove(struct map_session_data *sd, int achievement_id)
{
	struct achievement dummy;
	int i;

	nullpo_retr(false, sd);

	if (!achievement_exists(achievement_id)) {
		ShowError("achievement_delete: Achievement %d not found in DB.\n", achievement_id);
		return false;
	}

	ARR_FIND(0, sd->achievement_data.count, i, sd->achievement_data.achievements[i].achievement_id == achievement_id);
	if (i == sd->achievement_data.count) {
		ShowError("achievement_delete: Character %d doesn't have achievement %d.\n", sd->status.char_id, achievement_id);
		return false;
	}

	if (!sd->achievement_data.achievements[i].completed)
		sd->achievement_data.incompleteCount--;

	if (i != sd->achievement_data.count - 1)
		memmove(&sd->achievement_data.achievements[i], &sd->achievement_data.achievements[i + 1], sizeof(struct achievement) * (sd->achievement_data.count - 1 - i));

	sd->achievement_data.count--;
	if( sd->achievement_data.count == 0 ){
		aFree(sd->achievement_data.achievements);
		sd->achievement_data.achievements = NULL;
	}else{
		RECREATE(sd->achievement_data.achievements, struct achievement, sd->achievement_data.count);
	}
	sd->achievement_data.save = true;

	// Send a removed fake achievement
	memset(&dummy, 0, sizeof(struct achievement));
	dummy.achievement_id = achievement_id;
	clif_achievement_update(sd, &dummy, sd->achievement_data.count - sd->achievement_data.incompleteCount);

	return true;
}

/**
 * Lambda function that checks for completed achievements
 * @param sd: Player data
 * @param achievement_id: Achievement to check if it's complete
 * @return True on completed, false if not
 */
static bool achievement_done(struct map_session_data *sd, int achievement_id) {
	for (int i = 0; i < sd->achievement_data.count; i++) {
		if (sd->achievement_data.achievements[i].achievement_id == achievement_id && sd->achievement_data.achievements[i].completed > 0)
			return true;
	}

	return false;
}

/**
 * Checks to see if an achievement has a dependent, and if so, checks if that dependent is complete
 * @param sd: Player data
 * @param achievement_id: Achievement to check if it has a dependent
 * @return False on failure or not complete, true on complete or no dependents
 */
bool achievement_check_dependent(struct map_session_data *sd, int achievement_id)
{
	nullpo_retr(false, sd);

	if (!achievement_exists(achievement_id))
		return false;

	auto &adb = achievements[achievement_id];

	// Check if the achievement has a dependent
	// If so, then do a check on all dependents to see if they're complete
	for (int i = 0; i < adb->dependent_ids.size(); i++) {
		if (!achievement_done(sd, adb->dependent_ids[i]))
			return false; // One of the dependent is not complete!
	}

	return true;
}

/**
 * Check achievements that only have dependents and no other requirements
 * @param sd: Player to update
 * @param sd: Achievement to compare for completed dependents
 * @return True if successful, false if not
 */
static int achievement_check_groups(struct map_session_data *sd, struct s_achievement_db *ad)
{
	int i;

	if (ad == NULL || sd == NULL)
		return 0;

	if (ad->group != AG_BATTLE && ad->group != AG_TAMING && ad->group != AG_ADVENTURE)
		return 0;

	if (ad->dependent_ids.size() == 0 || ad->condition)
		return 0;

	ARR_FIND(0, sd->achievement_data.count, i, sd->achievement_data.achievements[i].achievement_id == ad->achievement_id);
	if (i == sd->achievement_data.count) { // Achievement isn't in player's log
		if (achievement_check_dependent(sd, ad->achievement_id) == true) {
			achievement_add(sd, ad->achievement_id);
			achievement_update_achievement(sd, ad->achievement_id, true);
		}
	}

	return 1;
}

/**
 * Update an achievement
 * @param sd: Player to update
 * @param achievement_id: Achievement ID of the achievement to update
 * @param complete: Complete state of an achievement
 * @return True if successful, false if not
 */
bool achievement_update_achievement(struct map_session_data *sd, int achievement_id, bool complete)
{
	int i;

	nullpo_retr(false, sd);

	if (!achievement_exists(achievement_id))
		return false;

	auto &adb = achievements[achievement_id];

	ARR_FIND(0, sd->achievement_data.incompleteCount, i, sd->achievement_data.achievements[i].achievement_id == achievement_id);
	if (i == sd->achievement_data.incompleteCount)
		return false;

	if (sd->achievement_data.achievements[i].completed > 0)
		return false;

	// Finally we send the updated achievement to the client
	if (complete) {
		if (adb->targets.size()) { // Make sure all the objective targets are at their respective total requirement
			int k;

			for (k = 0; k < adb->targets.size(); k++)
				sd->achievement_data.achievements[i].count[k] = adb->targets[k].count;

			for (k = 1; k < adb->dependent_ids.size(); k++) {
				sd->achievement_data.achievements[i].count[k] = max(1, sd->achievement_data.achievements[i].count[k]);
			}
		}

		sd->achievement_data.achievements[i].completed = time(NULL);

		if (i < (--sd->achievement_data.incompleteCount)) { // The achievement needs to be moved to the completed achievements block at the end of the array
			struct achievement tmp_ach;

			memcpy(&tmp_ach, &sd->achievement_data.achievements[i], sizeof(struct achievement));
			memcpy(&sd->achievement_data.achievements[i], &sd->achievement_data.achievements[sd->achievement_data.incompleteCount], sizeof(struct achievement));
			memcpy(&sd->achievement_data.achievements[sd->achievement_data.incompleteCount], &tmp_ach, sizeof(struct achievement));
		}

		achievement_level(sd, true); // Re-calculate achievement level
		// Check dependents
		for (auto &ach : achievements)
			achievement_check_groups(sd, ach.second.get());
		ARR_FIND(sd->achievement_data.incompleteCount, sd->achievement_data.count, i, sd->achievement_data.achievements[i].achievement_id == achievement_id); // Look for the index again, the position most likely changed
	}

	clif_achievement_update(sd, &sd->achievement_data.achievements[i], sd->achievement_data.count - sd->achievement_data.incompleteCount);
	sd->achievement_data.save = true; // Flag to save with the autosave interval

	return true;
}

/**
 * Get the reward of an achievement
 * @param sd: Player getting the reward
 * @param achievement_id: Achievement to get reward data
 */
void achievement_get_reward(struct map_session_data *sd, int achievement_id, time_t rewarded)
{
	int i;

	nullpo_retv(sd);

	if (!achievement_exists(achievement_id)) {
		ShowError("achievement_reward: Inter server sent a reward claim for achievement %d not found in DB.\n", achievement_id);
		return;
	}

	auto &adb = achievements[achievement_id];

	if (rewarded == 0) {
		clif_achievement_reward_ack(sd->fd, 0, achievement_id);
		return;
	}

	ARR_FIND(0, sd->achievement_data.count, i, sd->achievement_data.achievements[i].achievement_id == achievement_id);
	if (i == sd->achievement_data.count)
		return;

	// Only update in the cache, db was updated already
	sd->achievement_data.achievements[i].rewarded = rewarded;
	sd->achievement_data.save = true;

	run_script(adb->rewards.script, 0, sd->bl.id, fake_nd->bl.id);
	if (adb->rewards.title_id) {
		sd->titles.push_back(adb->rewards.title_id);
		clif_achievement_list_all(sd);
	}else{
		clif_achievement_reward_ack(sd->fd, 1, achievement_id);
		clif_achievement_update(sd, &sd->achievement_data.achievements[i], sd->achievement_data.count - sd->achievement_data.incompleteCount);
	}
}

/**
 * Check if player has recieved an achievement's reward
 * @param sd: Player to get reward
 * @param achievement_id: Achievement to get reward data
 */
void achievement_check_reward(struct map_session_data *sd, int achievement_id)
{
	int i;

	nullpo_retv(sd);

	if (!achievement_exists(achievement_id)) {
		ShowError("achievement_reward: Trying to reward achievement %d not found in DB.\n", achievement_id);
		clif_achievement_reward_ack(sd->fd, 0, achievement_id);
		return;
	}

	auto &adb = achievements[achievement_id];

	ARR_FIND(0, sd->achievement_data.count, i, sd->achievement_data.achievements[i].achievement_id == achievement_id);
	if (i == sd->achievement_data.count) {
		clif_achievement_reward_ack(sd->fd, 0, achievement_id);
		return;
	}

	if (sd->achievement_data.achievements[i].rewarded > 0 || sd->achievement_data.achievements[i].completed == 0) {
		clif_achievement_reward_ack(sd->fd, 0, achievement_id);
		return;
	}

	if (!intif_achievement_reward(sd, adb.get())) {
		clif_achievement_reward_ack(sd->fd, 0, achievement_id);
	}
}

/**
 * Return all titles to a player based on completed achievements
 * @param char_id: Character ID requesting
 */
void achievement_get_titles(uint32 char_id)
{
	struct map_session_data *sd = map_charid2sd(char_id);

	if (sd) {
		sd->titles.clear();

		if (sd->achievement_data.count) {
			for (int i = 0; i < sd->achievement_data.count; i++) {
				if (!achievement_exists(sd->achievement_data.achievements[i].achievement_id))
					continue;

				auto &adb = achievements[sd->achievement_data.achievements[i].achievement_id];

				if (adb && adb->rewards.title_id && sd->achievement_data.achievements[i].completed > 0) // If the achievement has a title and is complete, give it to the player
					sd->titles.push_back(adb->rewards.title_id);
			}
		}
	}
}

/**
 * Frees the player's data for achievements
 * @param sd: Player's session
 */
void achievement_free(struct map_session_data *sd)
{
	nullpo_retv(sd);

	if (sd->achievement_data.count) {
		aFree(sd->achievement_data.achievements);
		sd->achievement_data.achievements = NULL;
		sd->achievement_data.count = sd->achievement_data.incompleteCount = 0;
	}
}

/**
 * Get an achievement's progress information
 * @param sd: Player to check achievement progress
 * @param achievement_id: Achievement progress to check
 * @param type: Type to return
 * @return The type's data, -1 if player doesn't have achievement, -2 on failure/incorrect type
 */
int achievement_check_progress(struct map_session_data *sd, int achievement_id, int type)
{
	int i;

	nullpo_retr(-2, sd);

	// Achievement ID is not needed so skip the lookup
	if (type == ACHIEVEINFO_LEVEL)
		return sd->achievement_data.level;
	else if (type == ACHIEVEINFO_SCORE)
		return sd->achievement_data.total_score;

	ARR_FIND(0, sd->achievement_data.count, i, sd->achievement_data.achievements[i].achievement_id == achievement_id);
	if (i == sd->achievement_data.count)
		return -1;

	if (type >= ACHIEVEINFO_COUNT1 && type <= ACHIEVEINFO_COUNT10)
		return sd->achievement_data.achievements[i].count[type - 1];
	else if (type == ACHIEVEINFO_COMPLETE)
		return sd->achievement_data.achievements[i].completed > 0;
	else if (type == ACHIEVEINFO_COMPLETEDATE)
		return (int)sd->achievement_data.achievements[i].completed;
	else if (type == ACHIEVEINFO_GOTREWARD)
		return sd->achievement_data.achievements[i].rewarded > 0;
	return -2;
}

/**
 * Calculate a player's achievement level
 * @param sd: Player to check achievement level
 * @param flag: If the call should attempt to give the AG_GOAL_ACHIEVE achievement
 * @return Player's achievement level or 0 on failure
 */
int *achievement_level(struct map_session_data *sd, bool flag)
{
	static int info[2];
	int i, old_level;
	const int score_table[MAX_ACHIEVEMENT_RANK] = { 18, 31, 49, 73, 135, 104, 140, 178, 900, 1000, 1100, 1200, 1300, 1400, 1500, 1600, 1700, 1800, 1900, 2000 }; //! TODO: Figure out the EXP required to level up from 8-20

	nullpo_retr(0, sd);

	sd->achievement_data.total_score = 0;
	old_level = sd->achievement_data.level;

	for (i = 0; i < sd->achievement_data.count; i++) {
		if (sd->achievement_data.achievements[i].completed > 0)
			sd->achievement_data.total_score += sd->achievement_data.achievements[i].score;
	}

	info[0] = 0;
	info[1] = 0;

	for (i = 0; i < MAX_ACHIEVEMENT_RANK; i++) {
		info[0] = info[1];
			
		if (i < ARRAYLENGTH(score_table))
			info[1] = score_table[i];
		else {
			info[0] = info[1];
			info[1] = info[1] + 500;
		}

		if (sd->achievement_data.total_score < info[1])
			break;
	}

	if (i == MAX_ACHIEVEMENT_RANK)
		i = 0;

	info[1] = info[1] - info[0]; // Right number
	info[0] = sd->achievement_data.total_score - info[0]; // Left number
	sd->achievement_data.level = i;

	if (flag == true && old_level != sd->achievement_data.level) {
		int achievement_id = 240000 + sd->achievement_data.level;

		if( achievement_add(sd, achievement_id) ){
			achievement_update_achievement(sd, achievement_id, true);
		}
	}

	return info;
}

static bool achievement_check_condition( struct script_code* condition, struct map_session_data* sd, const int* count ){
	// Save the old script the player was attached to
	struct script_state* previous_st = sd->st;

	// Only if there was an old script
	if( previous_st != nullptr ){
		// Detach the player from the current script
		script_detach_rid(previous_st);
	}

	run_script( condition, 0, sd->bl.id, fake_nd->bl.id );

	struct script_state* st = sd->st;

	int value = 0;

	if( st != nullptr ){
		value = script_getnum( st, 2 );

		script_free_state(st);
	}

	// If an old script is present
	if( previous_st != nullptr ){
		// Because of detach the RID will be removed, so we need to restore it
		previous_st->rid = sd->bl.id;

		// Reattach the player to it, so that the limitations of that script kick back in
		script_attach_state( previous_st );
	}

	return value != 0;
}

/**
 * Update achievement objectives.
 * @param sd: Player to update
 * @param ad: Achievement data to compare for completion
 * @param group: Achievement group to update
 * @param update_count: Objective values player has
 * @return 1 on success and false on failure
 */
static int achievement_update_objectives(struct map_session_data *sd, std::shared_ptr<struct s_achievement_db> ad, enum e_achievement_group group, const std::array<int,MAX_ACHIEVEMENT_OBJECTIVES> &update_count)
{
	struct achievement *entry = NULL;
	bool isNew = false, changed = false, complete = false;
	int i, k = 0;
	std::array<int,MAX_ACHIEVEMENT_OBJECTIVES> objective_count;

	if (ad == NULL || sd == NULL)
		return 0;

	if (group <= AG_NONE || group >= AG_MAX)
		return 0;

	if (group != ad->group)
		return 0;

	objective_count.fill(0); // Current objectives count

	ARR_FIND(0, sd->achievement_data.count, i, sd->achievement_data.achievements[i].achievement_id == ad->achievement_id);
	if (i == sd->achievement_data.count) { // Achievement isn't in player's log
		if (achievement_check_dependent(sd, ad->achievement_id) == false) // Check to see if dependents are complete before adding to player's log
			return 0;
		isNew = true;
	} else {
		entry = &sd->achievement_data.achievements[i];

		if (entry->completed > 0) // Player has completed the achievement
			return 0;

		memcpy(objective_count.data(), entry->count, sizeof(objective_count));
	}

	switch (group) {
		case AG_ADD_FRIEND:
		case AG_BABY:
		case AG_CHAT_COUNT:
		case AG_CHAT_CREATE:
		case AG_CHAT_DYING:
		case AG_GET_ITEM:
		case AG_GET_ZENY:
		case AG_GOAL_LEVEL:
		case AG_GOAL_STATUS:
		case AG_JOB_CHANGE:
		case AG_MARRY:
		case AG_PARTY:
		case AG_REFINE_FAIL:
		case AG_REFINE_SUCCESS:
		case AG_SPEND_ZENY:
			if (group == AG_SPEND_ZENY) { // Achievement type is cummulative
				objective_count[0] += update_count[0];
				changed = true;
			}

			if (!ad->condition || achievement_check_condition(ad->condition, sd, update_count.data())) {
				changed = true;
				complete = true;
			}

			if (changed == false)
				break;

			if (isNew) {
				if ((entry = achievement_add(sd, ad->achievement_id)) == NULL)
					return 0; // Failed to add achievement, fall out
			}
			break;
		case AG_CHAT:
			if (!ad->targets.size())
				break;

			if (ad->condition && !achievement_check_condition(ad->condition, sd, update_count.data())) // Parameters weren't met
				break;

			if (ad->mapindex > -1 && sd->bl.m != ad->mapindex)
				break;

			for (i = 0; i < ad->targets.size(); i++) {
				if (objective_count[i] < ad->targets[i].count)
					objective_count[i] += update_count[0];
			}

			changed = true;

			ARR_FIND(0, ad->targets.size(), k, objective_count[k] < ad->targets[k].count);
			if (k == ad->targets.size())
				complete = true;

			if (isNew) {
				if ((entry = achievement_add(sd, ad->achievement_id)) == NULL)
					return 0; // Failed to add achievement, fall out
			}
			break;
		case AG_BATTLE:
		case AG_TAMING:
			auto it = std::find_if(ad->targets.begin(), ad->targets.end(), [&update_count]
			(const achievement_target &curTarget) {
				return curTarget.mob == update_count[0];
			});
			if (it == ad->targets.end())
				break; // Mob wasn't found

			for (k = 0; k < ad->targets.size(); k++) {
				if (ad->targets[k].mob == update_count[0] && objective_count[k] < ad->targets[k].count) {
					objective_count[k]++;
					changed = true;
				}
			}

			ARR_FIND(0, ad->targets.size(), k, objective_count[k] < ad->targets[k].count);
			if (k == ad->targets.size())
				complete = true;

			if (isNew) {
				if ((entry = achievement_add(sd, ad->achievement_id)) == NULL)
					return 0; // Failed to add achievement, fall out
			}
			break;
	}

	if (changed) {
		memcpy(entry->count, objective_count.data(), sizeof(objective_count));
		achievement_update_achievement(sd, ad->achievement_id, complete);
	}

	return 1;
}

/**
 * Update achievement objective count.
 * @param sd: Player data
 * @param group: Achievement enum type
 * @param sp_value: SP parameter value
 * @param arg_count: va_arg count
 */
void achievement_update_objective(struct map_session_data *sd, enum e_achievement_group group, uint8 arg_count, ...)
{
	if (sd) {
		va_list ap;
		int i;
		std::array<int,MAX_ACHIEVEMENT_OBJECTIVES> count;

		if (!battle_config.feature_achievement)
			return;

		count.fill(0); // Clear out array before setting values

		va_start(ap, arg_count);
		for (i = 0; i < arg_count; i++){
			std::string name = "ARG" + std::to_string(i);

			count[i] = va_arg(ap, int);

			pc_setglobalreg( sd, add_str( name.c_str() ), (int)count[i] );
		}
		va_end(ap);

		switch(group) {
			case AG_CHAT: //! TODO: Not sure how this works officially
			case AG_GOAL_ACHIEVE:
				// These have no objective use right now.
				break;
			default:
				for (auto &ach : achievements)
					achievement_update_objectives(sd, ach.second, group, count);
				break;
		}

		// Remove variables that might have been set
		for (i = 0; i < arg_count; i++){
			std::string name = "ARG" + std::to_string(i);

			pc_setglobalreg( sd, add_str( name.c_str() ), 0 );
		}
	}
}

static void yaml_invalid_warning(const char* fmt, const YAML::Node &node, const std::string &file) {
	YAML::Emitter out;
	out << node;
	ShowWarning(fmt, file.c_str());
	ShowMessage("%s\n", out.c_str());
}

/**
 * Reads and parses an entry from the achievement_db.
 * @param node: YAML node containing the entry.
 * @param n: The sequential index of the current entry.
 * @param source: The source YAML file.
 * @return True on successful parse or false otherwise
 */
bool achievement_read_db_sub(const YAML::Node &node, int n, const std::string &source)
{
	enum e_achievement_group group = AG_NONE;
	int achievement_id = 0;
	std::string group_char, name, condition, mapname;
	bool existing = false;

	if (!node["ID"]) {
		yaml_invalid_warning("achievement_read_db_sub: Missing ID field in '" CL_WHITE "%s" CL_RESET "', skipping.\n", node, source);
		return false;
	}
	try {
		achievement_id = node["ID"].as<unsigned int>();
	} catch (...) {
		yaml_invalid_warning("achievement_read_db_sub: Achievement definition with invalid ID field in '" CL_WHITE "%s" CL_RESET "', skipping.\n", node, source);
		return false;
	}
	if (achievement_id < 1 || achievement_id > INT_MAX) {
		ShowWarning("achievement_read_db_sub: Invalid achievement ID %d in \"%s\", entry #%d (min: 1, max: %d), skipping.\n", achievement_id, source.c_str(), n, INT_MAX);
		return false;
	}

	if (achievement_exists(achievement_id)) {
		if (source.find("import") != std::string::npos) // Import file read-in, free previous value and store new value
			existing = true;
		else { // Normal file read-in
			ShowWarning("achievement_read_db: Duplicate achievement %d.\n", achievement_id);
			return false;
		}
	}

	if(!existing)
		achievements[achievement_id] = std::make_shared<s_achievement_db>();
	auto &entry = achievements[achievement_id];
	entry->achievement_id = achievement_id;

	if (!node["Group"]) {
		yaml_invalid_warning("achievement_read_db_sub: Missing group field in '" CL_WHITE "%s" CL_RESET "', skipping.\n", node, source);
		return false;
	}
	try {
		group_char = node["Group"].as<std::string>();
	} catch (...) {
		yaml_invalid_warning("achievement_read_db_sub: Achievement definition with invalid group field in '" CL_WHITE "%s" CL_RESET "', skipping.\n", node, source);
		return false;
	}
	if (!script_get_constant(group_char.c_str(), (int *)&group)) {
		ShowWarning("achievement_read_db_sub: Invalid group %s for achievement %d in \"%s\", skipping.\n", group_char.c_str(), achievement_id, source.c_str());
		return false;
	}

	if (!node["Name"]) {
		ShowWarning("achievement_read_db_sub: Missing achievement name for achievement %d in \"%s\", skipping.\n", achievement_id, source.c_str());
		return false;
	}
	try {
		name = node["Name"].as<std::string>();
	} catch (...) {
		yaml_invalid_warning("achievement_read_db_sub: Achievement definition with invalid name field in '" CL_WHITE "%s" CL_RESET "', skipping.\n", node, source);
		return false;
	}
	
	entry->group = group;
	entry->name = name;
	entry->mapindex = -1;

	if (node["Target"]) {
		try {
			const YAML::Node &target_list = node["Target"];

			for (auto targetit = target_list.begin(); targetit != target_list.end() && target_list.size() < MAX_ACHIEVEMENT_OBJECTIVES; ++targetit) {
				const YAML::Node &target = *targetit;
				int mobid = 0, count = 0;

				if (target["MobID"] && (mobid = target["MobID"].as<int>()) && mob_db(mobid) == NULL) { // The mob ID field is not required
					ShowError("achievement_read_db_sub: Invalid mob ID %d for achievement %d in \"%s\", skipping.\n", mobid, achievement_id, source.c_str());
					continue;
				}
				if (target["Count"] && (!(count = target["Count"].as<int>()) || count <= 0)) {
					ShowError("achievement_read_db_sub: Invalid count %d for achievement %d in \"%s\", skipping.\n", count, achievement_id, source.c_str());
					continue;
				}
				if (mobid && group == AG_BATTLE && !achievement_mobexists(mobid))
					achievement_mobs.push_back(mobid);

				entry->targets.push_back({ mobid, count });
			}
		} catch (...) {
			yaml_invalid_warning("achievement_read_db_sub: Achievement definition with invalid target field in '" CL_WHITE "%s" CL_RESET "', skipping.\n", node, source);
			return false;
		}
	}

	if (node["Condition"]) {
		try {
			condition = node["Condition"].as<std::string>();
		} catch (...) {
			yaml_invalid_warning("achievement_read_db_sub: Achievement definition with invalid condition field in '" CL_WHITE "%s" CL_RESET "', skipping.\n", node, source);
			return false;
		}

		if( condition.find( "achievement_condition" ) == std::string::npos ){
			condition = "achievement_condition( " + condition + " );";
		}

		entry->condition = parse_script( condition.c_str(), source.c_str(), node["Condition"].Mark().line, SCRIPT_IGNORE_EXTERNAL_BRACKETS );
	}
	
	if (node["Map"]) {
		try {
			mapname = node["Map"].as<std::string>();
		} catch (...) {
			yaml_invalid_warning("achievement_read_db_sub: Achievement definition with invalid map field in '" CL_WHITE "%s" CL_RESET "', skipping.\n", node, source);
			return false;
		}
		if (group != AG_CHAT)
			ShowWarning("achievement_read_db_sub: The map argument can only be used with the group AG_CHATTING (achievement %d in \"%s\"), skipping.\n", achievement_id, source.c_str());
		else {
			entry->mapindex = map_mapname2mapid(mapname.c_str());

			if (entry->mapindex == -1)
				ShowWarning("achievement_read_db_sub: Invalid map name %s for achievement %d in \"%s\".\n", mapname.c_str(), achievement_id, source.c_str());
		}
	}

	if (node["Dependent"]) {
		try {
			const YAML::Node dependent_list = node["Dependent"];

			if (dependent_list.IsSequence()) {
				for (uint8 i = 0; i < dependent_list.size() && dependent_list.size() < MAX_ACHIEVEMENT_DEPENDENTS; i++)
					entry->dependent_ids.push_back(dependent_list[i].as<int>());
			} else
				ShowWarning("achievement_read_db_sub: Invalid dependent format for achievement %d in \"%s\".\n", achievement_id, source.c_str());
		} catch (...) {
			yaml_invalid_warning("achievement_read_db_sub: Achievement definition with invalid dependent field in '" CL_WHITE "%s" CL_RESET "', skipping.\n", node, source);
			return false;
		}
	}

	if (node["Reward"]) {
		try {
			const YAML::Node reward_list = node["Reward"];
			int nameid = 0, amount = 0, titleid = 0;

			if (reward_list["ItemID"] && (nameid = reward_list["ItemID"].as<unsigned short>())) {
				if (itemdb_exists(nameid)) {
					entry->rewards.nameid = nameid;
					entry->rewards.amount = 1; // Default the amount to 1
				} else if (nameid && !itemdb_exists(nameid)) {
					ShowWarning("achievement_read_db_sub: Invalid reward item ID %hu for achievement %d in \"%s\". Setting to 0.\n", nameid, achievement_id, source.c_str());
					entry->rewards.nameid = nameid = 0;
				}

				if (reward_list["Amount"] && (amount = reward_list["Amount"].as<unsigned short>()) && amount > 0 && nameid > 0)
					entry->rewards.amount = amount;
			}
			if (reward_list["Script"])
				entry->rewards.script = parse_script(reward_list["Script"].as<std::string>().c_str(), source.c_str(), achievement_id, SCRIPT_IGNORE_EXTERNAL_BRACKETS);
			if (reward_list["TitleID"] && (titleid = reward_list["TitleID"].as<int>()) && titleid > 0)
				entry->rewards.title_id = titleid;
		} catch (...) {
			yaml_invalid_warning("achievement_read_db_sub: Achievement definition with invalid target field in '" CL_WHITE "%s" CL_RESET "', skipping.\n", node, source);
			return false;
		}
	}

	if (node["Score"]) {
		try {
			entry->score = node["Score"].as<int>();
		} catch (...) {
			yaml_invalid_warning("achievement_read_db_sub: Achievement definition with invalid score field in '" CL_WHITE "%s" CL_RESET "', skipping.\n", node, source);
			return false;
		}
	}

	return true;
}

/**
 * Loads achievements from the achievement db.
 */
void achievement_read_db(void)
{
	std::vector<std::string> directories = { std::string(db_path) + "/" + std::string(DBPATH),  std::string(db_path) + "/" + std::string(DBIMPORT) + "/" };
	static const std::string file_name("achievement_db.yml");

	for (auto &directory : directories) {
		std::string current_file = directory + file_name;
		YAML::Node config;
		int count = 0;

		try {
			config = YAML::LoadFile(current_file);
		} catch (...) {
			ShowError("Cannot read '" CL_WHITE "%s" CL_RESET "'.\n", current_file.c_str());
			return;
		}

		for (const auto &node : config["Achievements"]) {
			if (node.IsDefined() && achievement_read_db_sub(node, count, current_file))
				count++;
		}
		for (auto &achit : achievements) {
			const auto ach = achit.second;

			for (int i = 0; i < ach->dependent_ids.size(); i++) {
				if (!achievement_exists(ach->dependent_ids[i])) {
					ShowWarning("achievement_read_db: An invalid Dependent ID %d was given for Achievement %d. Removing from list.\n", ach->dependent_ids[i], ach->achievement_id);
					ach->dependent_ids.erase(ach->dependent_ids.begin() + i);
				}
			}
		}
		ShowStatus("Done reading '" CL_WHITE "%d" CL_RESET "' entries in '" CL_WHITE "%s" CL_RESET "'\n", count, current_file.c_str());
	}

	return;
}

/**
 * Reloads the achievement database
 */
void achievement_db_reload(void)
{
	if (!battle_config.feature_achievement)
		return;
	do_final_achievement();
	do_init_achievement();
}

/**
 * Initializes the achievement database
 */
void do_init_achievement(void)
{
	if (!battle_config.feature_achievement)
		return;
	achievement_read_db();
}

/**
 * Finalizes the achievement database
 */
void do_final_achievement(void)
{
	achievement_mobs.clear();
	achievements.clear();
}

/**
 * Achievement constructor
 */
s_achievement_db::s_achievement_db()
	: achievement_id(0)
	, name("")
	, group()
	, targets()
	, dependent_ids()
	, condition(nullptr)
	, mapindex(0)
	, rewards()
	, score(0)
	, has_dependent(0)
{}

/**
* Achievement deconstructor
*/
s_achievement_db::~s_achievement_db()
{
	if (condition)
		script_free_code(condition);
}

/**
 * Achievement reward constructor
 */
s_achievement_db::ach_reward::ach_reward()
	: nameid(0)
	, amount(0)
	, script(nullptr)
	, title_id(0)
{}

/**
 * Achievement reward deconstructor
 */
s_achievement_db::ach_reward::~ach_reward()
{
	if (script)
		script_free_code(script);
}
