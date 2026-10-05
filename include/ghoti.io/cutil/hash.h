/*
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * Copyright (C) 2023-2026 Corey Pennycuff
 *
 * This file is part of Ghoti.io CUtil.
 *
 * Ghoti.io CUtil is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * Ghoti.io CUtil is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public
 * License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * @file
 * A simple hash table implementation.
 */

#ifndef GHOTI_IO_GCU_HASH_H
#define GHOTI_IO_GCU_HASH_H

#include <ghoti.io/cutil/macros.h>

#include <stddef.h>
#include <stdint.h>
#include <ghoti.io/cutil/type.h>
#include <ghoti.io/cutil/mutex.h>

#ifdef __cplusplus
extern "C" {
#endif


typedef struct GCU_Hash64 GCU_Hash64;
typedef struct GCU_Hash32 GCU_Hash32;
typedef struct GCU_Hash16 GCU_Hash16;
typedef struct GCU_Hash8 GCU_Hash8;

/**
 * Pointer to a function which will be called when the hash table destroy
 * function is called.
 *
 * `gcu_hash64_destroy()`
 *
 * @param hash table The hash table which is about to be destroyed.
 */
typedef void (* GCU_Hash64_Cleanup)(GCU_Hash64 * hashTable);

/**
 * Pointer to a function which will be called when the hash table destroy
 * function is called.
 *
 * `gcu_hash32_destroy()`
 *
 * @param hash table The hash table which is about to be destroyed.
 */
typedef void (* GCU_Hash32_Cleanup)(GCU_Hash32 * hashTable);

/**
 * Pointer to a function which will be called when the hash table destroy
 * function is called.
 *
 * `gcu_hash16_destroy()`
 *
 * @param hash table The hash table which is about to be destroyed.
 */
typedef void (* GCU_Hash16_Cleanup)(GCU_Hash16 * hashTable);

/**
 * Pointer to a function which will be called when the hash table destroy
 * function is called.
 *
 * `gcu_hash8_destroy()`
 *
 * @param hash table The hash table which is about to be destroyed.
 */
typedef void (* GCU_Hash8_Cleanup)(GCU_Hash8 * hashTable);

/**
 * 64-bit container used to return the result of looking for a hash in the hash
 * table.
 *
 * Although it may seem strange to return a value as part of a structure,
 * especially when the programmer undoubtedly just wants the value, it is also
 * imperitive that the hash table be able to indicate whether or not the value
 * existed in the table.  Both goals are accomplished by this approach.
 */
typedef struct {
  bool exists;            ///< Whether or not the value exists in the hash
                          ///<   table.
  GCU_Type64_Union value; ///< The value found in the table (if it exists).
} GCU_Hash64_Value;

/**
 * 64-bit container holding the information for an entry in the hash table.
 *
 * An "entry" is empty (e.g., `occupied = false`) upon creation.  By adding
 * and removing entries from the hash table, the `occupied` and `removed` flags
 * will be changed to track the state of each individual cell.
 */
typedef struct {
  size_t hash;           ///< The hash of the entry.
  GCU_Type64_Union data; ///< The data of the entry.
  bool occupied;         ///< Whether or not the entry has been initialized in
                         ///<   some way.
  bool removed;          ///< Whether or not the entry has been removed.
} GCU_Hash64_Cell;

/**
 * 64-bit container holding the information of the hash table.
 *
 * For proper memory management, the programmer is responsible for 4 things:
 *   1. Initialize the hash table using gcu_hash_create().
 *   2. Destroy the has table using gcu_hash_destory().
 *   3. Implementation of any thread-safety synchronization.
 *   4. Life cycle management of the contents of the hash table.  The hash
 *      table will **not**, for example, attempt to manage any pointers that it
 *      may contain upon deletion.  The programmer is responsible for all
 *      memory management.
 */
typedef struct GCU_Hash64 {
  size_t capacity;            ///< The total item capacity of the hash table.
  size_t entries;             ///< The count of non-empty cells.
  size_t removed;             ///< The count of non-empty cells that represent
                              ///<   elements which have been removed.
  GCU_Hash64_Cell * data;     ///< A pointer to the array of data cells.
  void * supplementary_data;  ///< User-defined.
  GCU_Hash64_Cleanup cleanup; ///< User-defined cleanup function.
  GCU_MUTEX_T mutex;          ///< Mutex for thread-safety.
} GCU_Hash64;

/**
 * A 64-bit container used to hold the state of an iterator which can be used
 * to traverse all elements of a hash table.
 *
 * A hash table may change internal structure upon adding or removing elements,
 * so any such operations may invalidate the behavior of an iterator.
 *
 * The programmer is responsible to make sure that an iterator is not used
 * improperly after the hash has been modified.
 *
 * An iterator may contain invalid data, in the case where there is no data
 * through which to iterate.  This is indicated by the `exists` field.  The
 * programmer is responsible for checking this field before attempting to
 * use the `value` in any way.
 */
typedef struct {
  size_t current;         ///< The current index into the hashTable data
                          ///<   structure corresponding to the iterator.
  bool exists;            ///< Whether or not the iterator points to valid data.
  size_t hash;            ///< The hash pointed to by the iterator.
  GCU_Type64_Union value; ///< The data pointed to by the iterator.
  GCU_Hash64 * hashTable; ///< The hash table that the iterator traverses.
} GCU_Hash64_Iterator;

/**
 * Create a hash table structure for 64-bit entries.
 *
 * All invocations of a hash table must have a corresponding
 * gcu_hash64_destroy() call in order to clean up dynamically-allocated memory.
 *
 * The hash table will manage the final size of container's memory based on the
 * number of elements that have been added.  The container's memory will be
 * expanded automatically when needed to accomodate new insertions, which can
 * cause an unexpected delay.  Such rebuilding costs can be avoided by proper
 * setting of the `count` variable during creation of the hash table.
 *
 * `count` is a reservation, and it is best-effort: a table whose cells could
 * not be allocated is returned empty rather than as a failure, and allocates
 * on its first insertion like any other.  Only a table that could not be
 * created at all is reported, by returning null.
 *
 * @param count The number of items anticipated to be stored in the hash table.
 * @return A struct containing the hash table information.
 */
GCU_API GCU_Hash64 * gcu_hash64_create(size_t count);

/**
 * Create a hash table structure for 64-bit entries in a pre-allocated memory
 * space.
 *
 * @param hash The hash table structure to be initialized.
 * @param count The number of items anticipated to be stored in the hash table,
 *   reserved best-effort as in gcu_hash64_create().
 * @return `true` on success, `false` on failure.  A reservation that could not
 *   be met is not a failure; only a table that could not be initialised is.
 */
GCU_API bool gcu_hash64_create_in_place(GCU_Hash64 * hash, size_t count);

/**
 * Destroy a hash table structure and clean up memory allocations.
 *
 * This function will not address any memory allocations of the elements
 * themselves (if any).  The programmer is responsible for controlling any
 * memory management on behalf of the elements.
 *
 * @param hashTable The hash table structure to be destroyed.
 */
GCU_API void gcu_hash64_destroy(GCU_Hash64 * hashTable);

/**
 * Destroy a hash table (except for the structure memory allocation).
 *
 * @param hashTable The hash table structure to be destroyed.
 */
GCU_API void gcu_hash64_destroy_in_place(GCU_Hash64 * hashTable);

/**
 * Clone a hash table structure.
 *
 * This function will create a new hash table with the same contents as the
 * source hash table.  The new hash table will have the same capacity as the
 * source hash table, but will not share any memory with the source hash table.
 * The new hash table will have a new mutex, and the `supplementary_data` and
 * `cleanup` fields will be copied from the source hash table.
 */
GCU_API GCU_Hash64 * gcu_hash64_clone(GCU_Hash64 * source);

/**
 * Set a value in the hash table.
 *
 * Setting a value may trigger a resize of the hash table.  This can be avoided
 * entirely by setting an appropriate `count` value when creating the hash
 * table with gcu_hash_create().
 *
 * @param hashTable The hash table structure on which to operate.
 * @param hash The hash associated with the value.
 * @param value The value to insert into the hash table.
 * @return `true` on success, `false` on failure.
 */
GCU_API bool gcu_hash64_set(GCU_Hash64 * hashTable, size_t hash, GCU_Type64_Union value);

/**
 * Get a value from the hash table (if it exists).
 *
 * @param hashTable The hash table structure on which to operate.
 * @param hash The hash whose associated value will be searched for.
 * @returns A result that indicates the success or failure of the operation, as
 *   well as the associated value (if it exists).
 */
GCU_API GCU_Hash64_Value gcu_hash64_get(GCU_Hash64 * hashTable, size_t hash);

/**
 * Check to see whether or not a hash table contains a specific hash.
 *
 * @param hashTable The hash table structure on which to operate.
 * @param hash The hash whose associated value will be searched for.
 * @return `true` if the hash is in the table, `false` otherwise.
 */
GCU_API bool gcu_hash64_contains(GCU_Hash64 * hashTable, size_t hash);

/**
 * Remove a hash from the table.
 *
 * The hash table does not manage the values in the table.  Therefore, if an
 * entry is removed from the hash table, then it is up to the programmer to
 * perform any additional work (such as memory cleanup of the value).
 *
 * @param hashTable The hash table structure on which to operate.
 * @param hash The hash whose associated value will be removed from the table.
 * @return `true` if the entry existed and was removed, `false` otherwise.
 */
GCU_API bool gcu_hash64_remove(GCU_Hash64 * hashTable, size_t hash);

/**
 * Get a count of active entries in the hash table.
 *
 * @param hashTable The hash table structure on which to operate.
 * @return The count of active entries in the hash table.
 */
GCU_API size_t gcu_hash64_count(GCU_Hash64 * hashTable);

/**
 * Get an iterator which can be used to iterate through the entries of the
 * hash table.
 *
 * @param hashTable The hash table structure on which to operate.
 * @return An iterator pointing to the first element in the hash table (if it
 *   exists).
 */
GCU_API GCU_Hash64_Iterator gcu_hash64_iterator_get(GCU_Hash64 * hashTable);

/**
 * Get an iterator to the next element in the table (if it exists).
 *
 * Any change to the hash table (such as setting a value) might alter the
 * underlying structure of the hash table, which would invalidate the iterator.
 * Any call to gcu_hash64_set(), therefore, should be considered as an
 * invalidation of any iterators associated with the hash table.
 *
 * @param iterator The iterator from which to calculate and return the
 *   next iterator.
 * @return An iterator pointing to the next element in the table (if it
 *   exists).
 */
GCU_API GCU_Hash64_Iterator gcu_hash64_iterator_next(GCU_Hash64_Iterator iterator);

/**
 * 32-bit container used to return the result of looking for a hash in the hash
 * table.
 *
 * Although it may seem strange to return a value as part of a structure,
 * especially when the programmer undoubtedly just wants the value, it is also
 * imperitive that the hash table be able to indicate whether or not the value
 * existed in the table.  Both goals are accomplished by this approach.
 */
typedef struct {
  bool exists;            ///< Whether or not the value exists in the hash
                          ///<   table.
  GCU_Type32_Union value; ///< The value found in the table (if it exists).
} GCU_Hash32_Value;

/**
 * 32-bit container holding the information for an entry in the hash table.
 *
 * An "entry" is empty (e.g., `occupied = false`) upon creation.  By adding
 * and removing entries from the hash table, the `occupied` and `removed` flags
 * will be changed to track the state of each individual cell.
 */
typedef struct {
  size_t hash;           ///< The hash of the entry.
  GCU_Type32_Union data; ///< The data of the entry.
  bool occupied;         ///< Whether or not the entry has been initialized in
                         ///<   some way.
  bool removed;          ///< Whether or not the entry has been removed.
} GCU_Hash32_Cell;

/**
 * 32-bit container holding the information of the hash table.
 *
 * For proper memory management, the programmer is responsible for 4 things:
 *   1. Initialize the hash table using gcu_hash32_create().
 *   2. Destroy the has table using gcu_hash32_destory().
 *   3. Implementation of any thread-safety synchronization.
 *   4. Life cycle management of the contents of the hash table.  The hash
 *      table will **not**, for example, attempt to manage any pointers that it
 *      may contain upon deletion.  The programmer is responsible for all
 *      memory management.
 */
typedef struct GCU_Hash32 {
  size_t capacity;            ///< The total item capacity of the hash table.
  size_t entries;             ///< The count of non-empty cells.
  size_t removed;             ///< The count of non-empty cells that represent
                              ///<   elements which have been removed.
  GCU_Hash32_Cell * data;     ///< A pointer to the array of data cells.
  void * supplementary_data;  ///< User-defined.
  GCU_Hash32_Cleanup cleanup; ///< User-defined cleanup function.
  GCU_MUTEX_T mutex;          ///< Mutex for thread-safety.
} GCU_Hash32;

/**
 * A container used to hold the state of an iterator which can be used to
 * traverse all elements of a hash table.
 *
 * A hash table may change internal structure upon adding or removing elements,
 * so any such operations may invalidate the behavior of an iterator.
 *
 * The programmer is responsible to make sure that an iterator is not used
 * improperly after the hash has been modified.
 *
 * An iterator may contain invalid data, in the case where there is no data
 * through which to iterate.  This is indicated by the `exists` field.  The
 * programmer is responsible for checking this field before attempting to
 * use the `value` in any way.
 */
typedef struct {
  size_t current;         ///< The current index into the hashTable data
                          ///<   structure corresponding to the iterator.
  bool exists;            ///< Whether or not the iterator points to valid data.
  size_t hash;            ///< The hash pointed to by the iterator.
  GCU_Type32_Union value; ///< The data pointed to by the iterator.
  GCU_Hash32 * hashTable; ///< The hash table that the iterator traverses.
} GCU_Hash32_Iterator;

/**
 * Create a hash table structure for 32-bit entries.
 *
 * All invocations of a hash table must have a corresponding
 * gcu_hash32_destroy() call in order to clean up dynamically-allocated memory.
 *
 * The hash table will manage the final size of container's memory based on the
 * number of elements that have been added.  The container's memory will be
 * expanded automatically when needed to accomodate new insertions, which can
 * cause an unexpected delay.  Such rebuilding costs can be avoided by proper
 * setting of the `count` variable during creation of the hash table.
 *
 * `count` is a reservation, and it is best-effort: a table whose cells could
 * not be allocated is returned empty rather than as a failure, and allocates
 * on its first insertion like any other.  Only a table that could not be
 * created at all is reported, by returning null.
 *
 * @param count The number of items anticipated to be stored in the hash table.
 * @return A struct containing the hash table information.
 */
GCU_API GCU_Hash32 * gcu_hash32_create(size_t count);

/**
 * Create a hash table structure for 32-bit entries in a pre-allocated memory
 * space.
 *
 * @param hash The hash table structure to be initialized.
 * @param count The number of items anticipated to be stored in the hash table,
 *   reserved best-effort as in gcu_hash32_create().
 * @return `true` on success, `false` on failure.  A reservation that could not
 *   be met is not a failure; only a table that could not be initialised is.
 */
GCU_API bool gcu_hash32_create_in_place(GCU_Hash32 * hash, size_t count);

/**
 * Destroy a hash table structure and clean up memory allocations.
 *
 * This function will not address any memory allocations of the elements
 * themselves (if any).  The programmer is responsible for controlling any
 * memory management on behalf of the elements.
 *
 * @param hashTable The hash table structure to be destroyed.
 */
GCU_API void gcu_hash32_destroy(GCU_Hash32 * hashTable);

/**
 * Destroy a hash table (except for the structure memory allocation).
 *
 * @param hashTable The hash table structure to be destroyed.
 */
GCU_API void gcu_hash32_destroy_in_place(GCU_Hash32 * hashTable);

/**
 * Clone a hash table structure.
 *
 * This function will create a new hash table with the same contents as the
 * source hash table.  The new hash table will have the same capacity as the
 * source hash table, but will not share any memory with the source hash table.
 * The new hash table will have a new mutex, and the `supplementary_data` and
 * `cleanup` fields will be copied from the source hash table.
 */
GCU_API GCU_Hash32 * gcu_hash32_clone(GCU_Hash32 * source);

/**
 * Set a value in the hash table.
 *
 * Setting a value may trigger a resize of the hash table.  This can be avoided
 * entirely by setting an appropriate `count` value when creating the hash
 * table with gcu_hash32_create().
 *
 * @param hashTable The hash table structure on which to operate.
 * @param hash The hash associated with the value.
 * @param value The value to insert into the hash table.
 * @return `true` on success, `false` on failure.
 */
GCU_API bool gcu_hash32_set(GCU_Hash32 * hashTable, size_t hash, GCU_Type32_Union value);

/**
 * Get a value from the hash table (if it exists).
 *
 * @param hashTable The hash table structure on which to operate.
 * @param hash The hash whose associated value will be searched for.
 * @returns A result that indicates the success or failure of the operation, as
 *   well as the associated value (if it exists).
 */
GCU_API GCU_Hash32_Value gcu_hash32_get(GCU_Hash32 * hashTable, size_t hash);

/**
 * Check to see whether or not a hash table contains a specific hash.
 *
 * @param hashTable The hash table structure on which to operate.
 * @param hash The hash whose associated value will be searched for.
 * @return `true` if the hash is in the table, `false` otherwise.
 */
GCU_API bool gcu_hash32_contains(GCU_Hash32 * hashTable, size_t hash);

/**
 * Remove a hash from the table.
 *
 * The hash table does not manage the values in the table.  Therefore, if an
 * entry is removed from the hash table, then it is up to the programmer to
 * perform any additional work (such as memory cleanup of the value).
 *
 * @param hashTable The hash table structure on which to operate.
 * @param hash The hash whose associated value will be removed from the table.
 * @return `true` if the entry existed and was removed, `false` otherwise.
 */
GCU_API bool gcu_hash32_remove(GCU_Hash32 * hashTable, size_t hash);

/**
 * Get a count of active entries in the hash table.
 *
 * @param hashTable The hash table structure on which to operate.
 * @return The count of active entries in the hash table.
 */
GCU_API size_t gcu_hash32_count(GCU_Hash32 * hashTable);

/**
 * Get an iterator which can be used to iterate through the entries of the
 * hash table.
 *
 * @param hashTable The hash table structure on which to operate.
 * @return An iterator pointing to the first element in the hash table (if it
 *   exists).
 */
GCU_API GCU_Hash32_Iterator gcu_hash32_iterator_get(GCU_Hash32 * hashTable);

/**
 * Get an iterator to the next element in the table (if it exists).
 *
 * Any change to the hash table (such as setting a value) might alter the
 * underlying structure of the hash table, which would invalidate the iterator.
 * Any call to gcu_hash32_set(), therefore, should be considered as an
 * invalidation of any iterators associated with the hash table.
 *
 * @param iterator The iterator from which to calculate and return the
 *   next iterator.
 * @return An iterator pointing to the next element in the table (if it
 *   exists).
 */
GCU_API GCU_Hash32_Iterator gcu_hash32_iterator_next(GCU_Hash32_Iterator iterator);

/**
 * 16-bit container used to return the result of looking for a hash in the hash
 * table.
 *
 * Although it may seem strange to return a value as part of a structure,
 * especially when the programmer undoubtedly just wants the value, it is also
 * imperitive that the hash table be able to indicate whether or not the value
 * existed in the table.  Both goals are accomplished by this approach.
 */
typedef struct {
  bool exists;            ///< Whether or not the value exists in the hash
                          ///<   table.
  GCU_Type16_Union value; ///< The value found in the table (if it exists).
} GCU_Hash16_Value;

/**
 * 16-bit container holding the information for an entry in the hash table.
 *
 * An "entry" is empty (e.g., `occupied = false`) upon creation.  By adding
 * and removing entries from the hash table, the `occupied` and `removed` flags
 * will be changed to track the state of each individual cell.
 */
typedef struct {
  size_t hash;           ///< The hash of the entry.
  GCU_Type16_Union data; ///< The data of the entry.
  bool occupied;         ///< Whether or not the entry has been initialized in
                         ///<   some way.
  bool removed;          ///< Whether or not the entry has been removed.
} GCU_Hash16_Cell;

/**
 * Container holding the information of the hash table.
 *
 * For proper memory management, the programmer is responsible for 4 things:
 *   1. Initialize the hash table using gcu_hash16_create().
 *   2. Destroy the has table using gcu_hash16_destory().
 *   3. Implementation of any thread-safety synchronization.
 *   4. Life cycle management of the contents of the hash table.  The hash
 *      table will **not**, for example, attempt to manage any pointers that it
 *      may contain upon deletion.  The programmer is responsible for all
 *      memory management.
 */
typedef struct GCU_Hash16 {
  size_t capacity;            ///< The total item capacity of the hash table.
  size_t entries;             ///< The count of non-empty cells.
  size_t removed;             ///< The count of non-empty cells that represent
                              ///<   elements which have been removed.
  GCU_Hash16_Cell * data;     ///< A pointer to the array of data cells.
  void * supplementary_data;  ///< User-defined.
  GCU_Hash16_Cleanup cleanup; ///< User-defined cleanup function.
  GCU_MUTEX_T mutex;          ///< Mutex for thread-safety.
} GCU_Hash16;

/**
 * A container used to hold the state of an iterator which can be used to
 * traverse all elements of a hash table.
 *
 * A hash table may change internal structure upon adding or removing elements,
 * so any such operations may invalidate the behavior of an iterator.
 *
 * The programmer is responsible to make sure that an iterator is not used
 * improperly after the hash has been modified.
 *
 * An iterator may contain invalid data, in the case where there is no data
 * through which to iterate.  This is indicated by the `exists` field.  The
 * programmer is responsible for checking this field before attempting to
 * use the `value` in any way.
 */
typedef struct {
  size_t current;         ///< The current index into the hashTable data
                          ///<   structure corresponding to the iterator.
  bool exists;            ///< Whether or not the iterator points to valid data.
  size_t hash;            ///< The hash pointed to by the iterator.
  GCU_Type16_Union value; ///< The data pointed to by the iterator.
  GCU_Hash16 * hashTable; ///< The hash table that the iterator traverses.
} GCU_Hash16_Iterator;

/**
 * Create a hash table structure.
 *
 * All invocations of a hash table must have a corresponding gcu_hash_destroy()
 * call in order to clean up dynamically-allocated memory.
 *
 * The hash table will manage the final size of container's memory based on the
 * number of elements that have been added.  The container's memory will be
 * expanded automatically when needed to accomodate new insertions, which can
 * cause an unexpected delay.  Such rebuilding costs can be avoided by proper
 * setting of the `count` variable during creation of the hash table.
 *
 * `count` is a reservation, and it is best-effort: a table whose cells could
 * not be allocated is returned empty rather than as a failure, and allocates
 * on its first insertion like any other.  Only a table that could not be
 * created at all is reported, by returning null.
 *
 * @param count The number of items anticipated to be stored in the hash table.
 * @return A struct containing the hash table information.
 */
GCU_API GCU_Hash16 * gcu_hash16_create(size_t count);

/**
 * Create a hash table structure in a pre-allocated memory space.
 *
 * @param hash The hash table structure to be initialized.
 * @param count The number of items anticipated to be stored in the hash table,
 *   reserved best-effort as in gcu_hash16_create().
 * @return `true` on success, `false` on failure.  A reservation that could not
 *   be met is not a failure; only a table that could not be initialised is.
 */
GCU_API bool gcu_hash16_create_in_place(GCU_Hash16 * hash, size_t count);

/**
 * Destroy a hash table structure and clean up memory allocations.
 *
 * This function will not address any memory allocations of the elements
 * themselves (if any).  The programmer is responsible for controlling any
 * memory management on behalf of the elements.
 *
 * @param hashTable The hash table structure to be destroyed.
 */
GCU_API void gcu_hash16_destroy(GCU_Hash16 * hashTable);

/**
 * Destroy a hash table (except for the structure memory allocation).
 *
 * @param hashTable The hash table structure to be destroyed.
 */
GCU_API void gcu_hash16_destroy_in_place(GCU_Hash16 * hashTable);

/**
 * Clone a hash table structure.
 *
 * This function will create a new hash table with the same contents as the
 * source hash table.  The new hash table will have the same capacity as the
 * source hash table, but will not share any memory with the source hash table.
 * The new hash table will have a new mutex, and the `supplementary_data` and
 * `cleanup` fields will be copied from the source hash table.
 */
GCU_API GCU_Hash16 * gcu_hash16_clone(GCU_Hash16 * source);

/**
 * Set a value in the hash table.
 *
 * Setting a value may trigger a resize of the hash table.  This can be avoided
 * entirely by setting an appropriate `count` value when creating the hash
 * table with gcu_hash16_create().
 *
 * @param hashTable The hash table structure on which to operate.
 * @param hash The hash associated with the value.
 * @param value The value to insert into the hash table.
 * @return `true` on success, `false` on failure.
 */
GCU_API bool gcu_hash16_set(GCU_Hash16 * hashTable, size_t hash, GCU_Type16_Union value);

/**
 * Get a value from the hash table (if it exists).
 *
 * @param hashTable The hash table structure on which to operate.
 * @param hash The hash whose associated value will be searched for.
 * @returns A result that indicates the success or failure of the operation, as
 *   well as the associated value (if it exists).
 */
GCU_API GCU_Hash16_Value gcu_hash16_get(GCU_Hash16 * hashTable, size_t hash);

/**
 * Check to see whether or not a hash table contains a specific hash.
 *
 * @param hashTable The hash table structure on which to operate.
 * @param hash The hash whose associated value will be searched for.
 * @return `true` if the hash is in the table, `false` otherwise.
 */
GCU_API bool gcu_hash16_contains(GCU_Hash16 * hashTable, size_t hash);

/**
 * Remove a hash from the table.
 *
 * The hash table does not manage the values in the table.  Therefore, if an
 * entry is removed from the hash table, then it is up to the programmer to
 * perform any additional work (such as memory cleanup of the value).
 *
 * @param hashTable The hash table structure on which to operate.
 * @param hash The hash whose associated value will be removed from the table.
 * @return `true` if the entry existed and was removed, `false` otherwise.
 */
GCU_API bool gcu_hash16_remove(GCU_Hash16 * hashTable, size_t hash);

/**
 * Get a count of active entries in the hash table.
 *
 * @param hashTable The hash table structure on which to operate.
 * @return The count of active entries in the hash table.
 */
GCU_API size_t gcu_hash16_count(GCU_Hash16 * hashTable);

/**
 * Get an iterator which can be used to iterate through the entries of the
 * hash table.
 *
 * @param hashTable The hash table structure on which to operate.
 * @return An iterator pointing to the first element in the hash table (if it
 *   exists).
 */
GCU_API GCU_Hash16_Iterator gcu_hash16_iterator_get(GCU_Hash16 * hashTable);

/**
 * Get an iterator to the next element in the table (if it exists).
 *
 * Any change to the hash table (such as setting a value) might alter the
 * underlying structure of the hash table, which would invalidate the iterator.
 * Any call to gcu_hash16_set(), therefore, should be considered as an
 * invalidation of any iterators associated with the hash table.
 *
 * @param iterator The iterator from which to calculate and return the
 *   next iterator.
 * @return An iterator pointing to the next element in the table (if it
 *   exists).
 */
GCU_API GCU_Hash16_Iterator gcu_hash16_iterator_next(GCU_Hash16_Iterator iterator);

/**
 * 8-bit container used to return the result of looking for a hash in the hash
 * table.
 *
 * Although it may seem strange to return a value as part of a structure,
 * especially when the programmer undoubtedly just wants the value, it is also
 * imperitive that the hash table be able to indicate whether or not the value
 * existed in the table.  Both goals are accomplished by this approach.
 */
typedef struct {
  bool exists;           ///< Whether or not the value exists in the hash table.
  GCU_Type8_Union value; ///< The value found in the table (if it exists).
} GCU_Hash8_Value;

/**
 * 8-bit container holding the information for an entry in the hash table.
 *
 * An "entry" is empty (e.g., `occupied = false`) upon creation.  By adding
 * and removing entries from the hash table, the `occupied` and `removed` flags
 * will be changed to track the state of each individual cell.
 */
typedef struct {
  size_t hash;          ///< The hash of the entry.
  GCU_Type8_Union data; ///< The data of the entry.
  bool occupied;        ///< Whether or not the entry has been initialized in
                        ///<   some way.
  bool removed;         ///< Whether or not the entry has been removed.
} GCU_Hash8_Cell;

/**
 * Container holding the information of the hash table.
 *
 * For proper memory management, the programmer is responsible for 4 things:
 *   1. Initialize the hash table using gcu_hash8_create().
 *   2. Destroy the has table using gcu_hash8_destory().
 *   3. Implementation of any thread-safety synchronization.
 *   4. Life cycle management of the contents of the hash table.  The hash
 *      table will **not**, for example, attempt to manage any pointers that it
 *      may contain upon deletion.  The programmer is responsible for all
 *      memory management.
 */
typedef struct GCU_Hash8 {
  size_t capacity;           ///< The total item capacity of the hash table.
  size_t entries;            ///< The count of non-empty cells.
  size_t removed;            ///< The count of non-empty cells that represent
                             ///<   elements which have been removed.
  GCU_Hash8_Cell * data;     ///< A pointer to the array of data cells.
  void * supplementary_data; ///< User-defined.
  GCU_Hash8_Cleanup cleanup; ///< User-defined cleanup function.
  GCU_MUTEX_T mutex;         ///< Mutex for thread-safety.
} GCU_Hash8;

/**
 * A container used to hold the state of an iterator which can be used to
 * traverse all elements of a hash table.
 *
 * A hash table may change internal structure upon adding or removing elements,
 * so any such operations may invalidate the behavior of an iterator.
 *
 * The programmer is responsible to make sure that an iterator is not used
 * improperly after the hash has been modified.
 *
 * An iterator may contain invalid data, in the case where there is no data
 * through which to iterate.  This is indicated by the `exists` field.  The
 * programmer is responsible for checking this field before attempting to
 * use the `value` in any way.
 */
typedef struct {
  size_t current;        ///< The current index into the hashTable data
                         ///<   structure corresponding to the iterator.
  bool exists;           ///< Whether or not the iterator points to valid data.
  size_t hash;           ///< The hash pointed to by the iterator.
  GCU_Type8_Union value; ///< The data pointed to by the iterator.
  GCU_Hash8 * hashTable; ///< The hash table that the iterator traverses.
} GCU_Hash8_Iterator;

/**
 * Create a hash table structure.
 *
 * All invocations of a hash table must have a corresponding gcu_hash8_destroy()
 * call in order to clean up dynamically-allocated memory.
 *
 * The hash table will manage the final size of container's memory based on the
 * number of elements that have been added.  The container's memory will be
 * expanded automatically when needed to accomodate new insertions, which can
 * cause an unexpected delay.  Such rebuilding costs can be avoided by proper
 * setting of the `count` variable during creation of the hash table.
 *
 * `count` is a reservation, and it is best-effort: a table whose cells could
 * not be allocated is returned empty rather than as a failure, and allocates
 * on its first insertion like any other.  Only a table that could not be
 * created at all is reported, by returning null.
 *
 * @param count The number of items anticipated to be stored in the hash table.
 * @return A struct containing the hash table information.
 */
GCU_API GCU_Hash8 * gcu_hash8_create(size_t count);

/**
 * Create a hash table structure in a pre-allocated memory space.
 *
 * @param hash The hash table structure to be initialized.
 * @param count The number of items anticipated to be stored in the hash table,
 *   reserved best-effort as in gcu_hash8_create().
 * @return `true` on success, `false` on failure.  A reservation that could not
 *   be met is not a failure; only a table that could not be initialised is.
 */
GCU_API bool gcu_hash8_create_in_place(GCU_Hash8 * hash, size_t count);

/**
 * Destroy a hash table structure and clean up memory allocations.
 *
 * This function will not address any memory allocations of the elements
 * themselves (if any).  The programmer is responsible for controlling any
 * memory management on behalf of the elements.
 *
 * @param hashTable The hash table structure to be destroyed.
 */
GCU_API void gcu_hash8_destroy(GCU_Hash8 * hashTable);

/**
 * Destroy a hash table (except for the structure memory allocation).
 *
 * @param hashTable The hash table structure to be destroyed.
 */
GCU_API void gcu_hash8_destroy_in_place(GCU_Hash8 * hashTable);

/**
 * Clone a hash table structure.
 *
 * This function will create a new hash table with the same contents as the
 * source hash table.  The new hash table will have the same capacity as the
 * source hash table, but will not share any memory with the source hash table.
 * The new hash table will have a new mutex, and the `supplementary_data` and
 * `cleanup` fields will be copied from the source hash table.
 */
GCU_API GCU_Hash8 * gcu_hash8_clone(GCU_Hash8 * source);

/**
 * Set a value in the hash table.
 *
 * Setting a value may trigger a resize of the hash table.  This can be avoided
 * entirely by setting an appropriate `count` value when creating the hash
 * table with gcu_hash8_create().
 *
 * @param hashTable The hash table structure on which to operate.
 * @param hash The hash associated with the value.
 * @param value The value to insert into the hash table.
 * @return `true` on success, `false` on failure.
 */
GCU_API bool gcu_hash8_set(GCU_Hash8 * hashTable, size_t hash, GCU_Type8_Union value);

/**
 * Get a value from the hash table (if it exists).
 *
 * @param hashTable The hash table structure on which to operate.
 * @param hash The hash whose associated value will be searched for.
 * @returns A result that indicates the success or failure of the operation, as
 *   well as the associated value (if it exists).
 */
GCU_API GCU_Hash8_Value gcu_hash8_get(GCU_Hash8 * hashTable, size_t hash);

/**
 * Check to see whether or not a hash table contains a specific hash.
 *
 * @param hashTable The hash table structure on which to operate.
 * @param hash The hash whose associated value will be searched for.
 * @return `true` if the hash is in the table, `false` otherwise.
 */
GCU_API bool gcu_hash8_contains(GCU_Hash8 * hashTable, size_t hash);

/**
 * Remove a hash from the table.
 *
 * The hash table does not manage the values in the table.  Therefore, if an
 * entry is removed from the hash table, then it is up to the programmer to
 * perform any additional work (such as memory cleanup of the value).
 *
 * @param hashTable The hash table structure on which to operate.
 * @param hash The hash whose associated value will be removed from the table.
 * @return `true` if the entry existed and was removed, `false` otherwise.
 */
GCU_API bool gcu_hash8_remove(GCU_Hash8 * hashTable, size_t hash);

/**
 * Get a count of active entries in the hash table.
 *
 * @param hashTable The hash table structure on which to operate.
 * @return The count of active entries in the hash table.
 */
GCU_API size_t gcu_hash8_count(GCU_Hash8 * hashTable);

/**
 * Get an iterator which can be used to iterate through the entries of the
 * hash table.
 *
 * @param hashTable The hash table structure on which to operate.
 * @return An iterator pointing to the first element in the hash table (if it
 *   exists).
 */
GCU_API GCU_Hash8_Iterator gcu_hash8_iterator_get(GCU_Hash8 * hashTable);

/**
 * Get an iterator to the next element in the table (if it exists).
 *
 * Any change to the hash table (such as setting a value) might alter the
 * underlying structure of the hash table, which would invalidate the iterator.
 * Any call to gcu_hash8_set(), therefore, should be considered as an
 * invalidation of any iterators associated with the hash table.
 *
 * @param iterator The iterator from which to calculate and return the
 *   next iterator.
 * @return An iterator pointing to the next element in the table (if it
 *   exists).
 */
GCU_API GCU_Hash8_Iterator gcu_hash8_iterator_next(GCU_Hash8_Iterator iterator);

#ifdef __cplusplus
}
#endif

#endif //GHOTI_IO_GCU_HASH_H

