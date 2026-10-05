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
 * A simple vector implementation.
 */

#ifndef GHOTI_IO_GCU_VECTOR_H
#define GHOTI_IO_GCU_VECTOR_H

#include <ghoti.io/cutil/macros.h>

#include <stddef.h>
#include <ghoti.io/cutil/type.h>
#include <ghoti.io/cutil/mutex.h>

#ifdef __cplusplus
extern "C" {
#endif


typedef struct GCU_Vector64 GCU_Vector64;
typedef struct GCU_Vector32 GCU_Vector32;
typedef struct GCU_Vector16 GCU_Vector16;
typedef struct GCU_Vector8 GCU_Vector8;

/**
 * Pointer to a function which will be called when the vector destroy function
 * is called.
 *
 * `gcu_vector64_destroy()`
 *
 * @param vector The vector which is about to be destroyed.
 */
typedef void (* GCU_Vector64_Cleanup)(GCU_Vector64 * vector);

/**
 * Pointer to a function which will be called when the vector destroy function
 * is called.
 *
 * `gcu_vector32_destroy()`
 *
 * @param vector The vector which is about to be destroyed.
 */
typedef void (* GCU_Vector32_Cleanup)(GCU_Vector32 * vector);

/**
 * Pointer to a function which will be called when the vector destroy function
 * is called.
 *
 * `gcu_vector16_destroy()`
 *
 * @param vector The vector which is about to be destroyed.
 */
typedef void (* GCU_Vector16_Cleanup)(GCU_Vector16 * vector);

/**
 * Pointer to a function which will be called when the vector destroy function
 * is called.
 *
 * `gcu_vector8_destroy()`
 *
 * @param vector The vector which is about to be destroyed.
 */
typedef void (* GCU_Vector8_Cleanup)(GCU_Vector8 * vector);

/**
 * Container holding the information of the 64-bit vector.
 *
 * For proper memory management, the programmer is responsible for 4 things:
 *   1. Initialize the vector using gcu_vector64_create().
 *   2. Destroy the vector using gcu_vector64_destroy().
 *   3. Implementation of any thread-safety synchronization.
 *   4. Life cycle management of the contents of the vector.  The vector
 *      will **not**, for example, attempt to manage any pointers that it
 *      may contain upon deletion.  The programmer is responsible for all
 *      memory management.
 *
 * The programmer may populate the `supplementary_data` data variable and
 * the `cleanup` function pointer.  When the vector is destroyed, the `cleanup`
 * function will be called (if provided).
 */
typedef struct GCU_Vector64 {
  size_t capacity;              ///< The total item capacity of the vector.
  size_t count;                 ///< The count of non-empty cells.
  GCU_Type64_Union * data;      ///< A pointer to the array of data cells.
  void * supplementary_data;    ///< User-defined.
  GCU_Vector64_Cleanup cleanup; ///< User-defined cleanup function.
  GCU_MUTEX_T mutex;            ///< Mutex for thread-safety.
} GCU_Vector64;

/**
 * Create a vector structure.
 *
 * All invocations of a vector must have a corresponding gcu_vector64_destroy()
 * call in order to clean up dynamically-allocated memory.
 *
 * The vector will manage the final size of container's memory based on the
 * number of elements that have been added.  The container's memory will be
 * expanded automatically when needed to accomodate new insertions, which can
 * cause an unexpected delay.  Such rebuilding costs can be avoided by proper
 * setting of the `count` variable during creation of the vector.
 *
 * @param count The number of items anticipated to be stored in the vector.
 * @return A struct containing the vector information.
 */
GCU_API GCU_Vector64 * gcu_vector64_create(size_t count);

/**
 * Create a vector structure in place.
 *
 * This function uses the provided memory to create the vector.  The memory
 * must be large enough to hold the vector structure.
 *
 * @param vector The memory to use for the vector.
 * @param count The number of items anticipated to be stored in the vector.
 * @return `true` on success, `false` otherwise.
 */
GCU_API bool gcu_vector64_create_in_place(GCU_Vector64 * vector, size_t count);

/**
 * Destroy a vector structure and clean up memory allocations.
 *
 * This function will not address any memory allocations of the elements
 * themselves (if any).  The programmer is responsible for controlling any
 * memory management on behalf of the elements.
 *
 * @param vector The vector structure to be destroyed.
 */
GCU_API void gcu_vector64_destroy(GCU_Vector64 * vector);

/**
 * Destroy a vector structure (except for the memory allocation).
 *
 * This function will not address the memory allocation of the vector struct.
 *
 * @param vector The vector structure to be destroyed.
 */
GCU_API void gcu_vector64_destroy_in_place(GCU_Vector64 * vector);

/**
 * Append an item at the end of the vector.
 *
 * If there is not enough space in the current data structure, new space will
 * be attempted to be allocated.  This may invalidate any pointers to the
 * previous data locations.
 *
 * @param vector The vector structure on which to operate.
 * @param value The item to append to the end of the vector.
 * @return `true` on success, `false` otherwise.
 */
GCU_API bool gcu_vector64_append(GCU_Vector64 * vector, GCU_Type64_Union value);

/**
 * Get a count of entries in the vector.
 *
 * @param vector The vector structure on which to operate.
 * @return The count of entries in the vector.
 */
GCU_API size_t gcu_vector64_count(GCU_Vector64 * vector);

/**
 * Reserve space in the vector.
 *
 * If the vector is larger than the requested amount, then nothing will be
 * done.  If the vector is smaller than the requested amount, then the vector
 * capacity will be increased to the requested amount (if possible).
 *
 * @param vector The vector structure on which to operate.
 * @param count The number of items to reserve.
 * @return `true` on success, `false` otherwise.
 */
GCU_API bool gcu_vector64_reserve(GCU_Vector64 * vector, size_t count);

/**
 * Container holding the information of the 32-bit vector.
 *
 * For proper memory management, the programmer is responsible for 4 things:
 *   1. Initialize the vector using gcu_vector32_create().
 *   2. Destroy the vector using gcu_vector32_destroy().
 *   3. Implementation of any thread-safety synchronization.
 *   4. Life cycle management of the contents of the vector.  The vector
 *      will **not**, for example, attempt to manage any pointers that it
 *      may contain upon deletion.  The programmer is responsible for all
 *      memory management.
 */
typedef struct GCU_Vector32 {
  size_t capacity;              ///< The total item capacity of the vector.
  size_t count;                 ///< The count of non-empty cells.
  GCU_Type32_Union * data;      ///< A pointer to the array of data cells.
  void * supplementary_data;    ///< User-defined.
  GCU_Vector32_Cleanup cleanup; ///< User-defined cleanup function.
  GCU_MUTEX_T mutex;            ///< Mutex for thread-safety.
} GCU_Vector32;

/**
 * Create a vector structure.
 *
 * All invocations of a vector must have a corresponding gcu_vector32_destroy()
 * call in order to clean up dynamically-allocated memory.
 *
 * The vector will manage the final size of container's memory based on the
 * number of elements that have been added.  The container's memory will be
 * expanded automatically when needed to accomodate new insertions, which can
 * cause an unexpected delay.  Such rebuilding costs can be avoided by proper
 * setting of the `count` variable during creation of the vector.
 *
 * @param count The number of items anticipated to be stored in the vector.
 * @return A struct containing the vector information.
 */
GCU_API GCU_Vector32 * gcu_vector32_create(size_t count);

/**
 * Create a vector structure in place.
 *
 * This function uses the provided memory to create the vector.  The memory
 * must be large enough to hold the vector structure.
 *
 * @param vector The memory to use for the vector.
 * @param count The number of items anticipated to be stored in the vector.
 * @return `true` on success, `false` otherwise.
 */
GCU_API bool gcu_vector32_create_in_place(GCU_Vector32 * vector, size_t count);

/**
 * Destroy a vector structure and clean up memory allocations.
 *
 * This function will not address any memory allocations of the elements
 * themselves (if any).  The programmer is responsible for controlling any
 * memory management on behalf of the elements.
 *
 * @param vector The vector structure to be destroyed.
 */
GCU_API void gcu_vector32_destroy(GCU_Vector32 * vector);

/**
 * Destroy a vector structure (except for the memory allocation).
 *
 * This function will not address the memory allocation of the vector struct.
 *
 * @param vector The vector structure to be destroyed.
 */
GCU_API void gcu_vector32_destroy_in_place(GCU_Vector32 * vector);

/**
 * Append an item at the end of the vector.
 *
 * If there is not enough space in the current data structure, new space will
 * be attempted to be allocated.  This may invalidate any pointers to the
 * previous data locations.
 *
 * @param vector The vector structure on which to operate.
 * @param value The item to append to the end of the vector.
 * @return `true` on success, `false` otherwise.
 */
GCU_API bool gcu_vector32_append(GCU_Vector32 * vector, GCU_Type32_Union value);

/**
 * Get a count of entries in the vector.
 *
 * @param vector The vector structure on which to operate.
 * @return The count of entries in the vector.
 */
GCU_API size_t gcu_vector32_count(GCU_Vector32 * vector);

/**
 * Reserve space in the vector.
 *
 * If the vector is larger than the requested amount, then nothing will be
 * done.  If the vector is smaller than the requested amount, then the vector
 * capacity will be increased to the requested amount (if possible).
 *
 * @param vector The vector structure on which to operate.
 * @param count The number of items to reserve.
 * @return `true` on success, `false` otherwise.
 */
GCU_API bool gcu_vector32_reserve(GCU_Vector32 * vector, size_t count);

/**
 * Container holding the information of the 16-bit vector.
 *
 * For proper memory management, the programmer is responsible for 4 things:
 *   1. Initialize the vector using gcu_vector16_create().
 *   2. Destroy the vector using gcu_vector16_destroy().
 *   3. Implementation of any thread-safety synchronization.
 *   4. Life cycle management of the contents of the vector.  The vector
 *      will **not**, for example, attempt to manage any pointers that it
 *      may contain upon deletion.  The programmer is responsible for all
 *      memory management.
 */
typedef struct GCU_Vector16 {
  size_t capacity;              ///< The total item capacity of the vector.
  size_t count;                 ///< The count of non-empty cells.
  GCU_Type16_Union * data;      ///< A pointer to the array of data cells.
  void * supplementary_data;    ///< User-defined.
  GCU_Vector16_Cleanup cleanup; ///< User-defined cleanup function.
  GCU_MUTEX_T mutex;            ///< Mutex for thread-safety.
} GCU_Vector16;

/**
 * Create a vector structure.
 *
 * All invocations of a vector must have a corresponding gcu_vector16_destroy()
 * call in order to clean up dynamically-allocated memory.
 *
 * The vector will manage the final size of container's memory based on the
 * number of elements that have been added.  The container's memory will be
 * expanded automatically when needed to accomodate new insertions, which can
 * cause an unexpected delay.  Such rebuilding costs can be avoided by proper
 * setting of the `count` variable during creation of the vector.
 *
 * @param count The number of items anticipated to be stored in the vector.
 * @return A struct containing the vector information.
 */
GCU_API GCU_Vector16 * gcu_vector16_create(size_t count);

/**
 * Create a vector structure in place.
 *
 * This function uses the provided memory to create the vector.  The memory
 * must be large enough to hold the vector structure.
 *
 * @param vector The memory to use for the vector.
 * @param count The number of items anticipated to be stored in the vector.
 * @return `true` on success, `false` otherwise.
 */
GCU_API bool gcu_vector16_create_in_place(GCU_Vector16 * vector, size_t count);

/**
 * Destroy a vector structure and clean up memory allocations.
 *
 * This function will not address any memory allocations of the elements
 * themselves (if any).  The programmer is responsible for controlling any
 * memory management on behalf of the elements.
 *
 * @param vector The vector structure to be destroyed.
 */
GCU_API void gcu_vector16_destroy(GCU_Vector16 * vector);

/**
 * Destroy a vector structure (except for the memory allocation).
 *
 * This function will not address the memory allocation of the vector struct.
 *
 * @param vector The vector structure to be destroyed.
 */
GCU_API void gcu_vector16_destroy_in_place(GCU_Vector16 * vector);

/**
 * Append an item at the end of the vector.
 *
 * If there is not enough space in the current data structure, new space will
 * be attempted to be allocated.  This may invalidate any pointers to the
 * previous data locations.
 *
 * @param vector The vector structure on which to operate.
 * @param value The item to append to the end of the vector.
 * @return `true` on success, `false` otherwise.
 */
GCU_API bool gcu_vector16_append(GCU_Vector16 * vector, GCU_Type16_Union value);

/**
 * Get a count of entries in the vector.
 *
 * @param vector The vector structure on which to operate.
 * @return The count of entries in the vector.
 */
GCU_API size_t gcu_vector16_count(GCU_Vector16 * vector);

/**
 * Reserve space in the vector.
 *
 * If the vector is larger than the requested amount, then nothing will be
 * done.  If the vector is smaller than the requested amount, then the vector
 * capacity will be increased to the requested amount (if possible).
 *
 * @param vector The vector structure on which to operate.
 * @param count The number of items to reserve.
 * @return `true` on success, `false` otherwise.
 */
GCU_API bool gcu_vector16_reserve(GCU_Vector16 * vector, size_t count);

/**
 * Container holding the information of the 8-bit vector.
 *
 * For proper memory management, the programmer is responsible for 4 things:
 *   1. Initialize the vector using gcu_vector8_create().
 *   2. Destroy the vector using gcu_vector8_destroy().
 *   3. Implementation of any thread-safety synchronization.
 *   4. Life cycle management of the contents of the vector.  The vector
 *      will **not**, for example, attempt to manage any pointers that it
 *      may contain upon deletion.  The programmer is responsible for all
 *      memory management.
 */
typedef struct GCU_Vector8 {
  size_t capacity;             ///< The total item capacity of the vector.
  size_t count;                ///< The count of non-empty cells.
  GCU_Type8_Union * data;      ///< A pointer to the array of data cells.
  void * supplementary_data;   ///< User-defined.
  GCU_Vector8_Cleanup cleanup; ///< User-defined cleanup function.
  GCU_MUTEX_T mutex;           ///< Mutex for thread-safety.
} GCU_Vector8;

/**
 * Create a vector structure.
 *
 * All invocations of a vector must have a corresponding gcu_vector8_destroy()
 * call in order to clean up dynamically-allocated memory.
 *
 * The vector will manage the final size of container's memory based on the
 * number of elements that have been added.  The container's memory will be
 * expanded automatically when needed to accomodate new insertions, which can
 * cause an unexpected delay.  Such rebuilding costs can be avoided by proper
 * setting of the `count` variable during creation of the vector.
 *
 * @param count The number of items anticipated to be stored in the vector.
 * @return A struct containing the vector information.
 */
GCU_API GCU_Vector8 * gcu_vector8_create(size_t count);

/**
 * Create a vector structure in place.
 *
 * This function uses the provided memory to create the vector.  The memory
 * must be large enough to hold the vector structure.
 *
 * @param vector The memory to use for the vector.
 * @param count The number of items anticipated to be stored in the vector.
 * @return `true` on success, `false` otherwise.
 */
GCU_API bool gcu_vector8_create_in_place(GCU_Vector8 * vector, size_t count);

/**
 * Destroy a vector structure and clean up memory allocations.
 *
 * This function will not address any memory allocations of the elements
 * themselves (if any).  The programmer is responsible for controlling any
 * memory management on behalf of the elements.
 *
 * @param vector The vector structure to be destroyed.
 */
GCU_API void gcu_vector8_destroy(GCU_Vector8 * vector);

/**
 * Destroy a vector structure (except for the memory allocation).
 *
 * This function will not address the memory allocation of the vector struct.
 *
 * @param vector The vector structure to be destroyed.
 */
GCU_API void gcu_vector8_destroy_in_place(GCU_Vector8 * vector);

/**
 * Append an item at the end of the vector.
 *
 * If there is not enough space in the current data structure, new space will
 * be attempted to be allocated.  This may invalidate any pointers to the
 * previous data locations.
 *
 * @param vector The vector structure on which to operate.
 * @param value The item to append to the end of the vector.
 * @return `true` on success, `false` otherwise.
 */
GCU_API bool gcu_vector8_append(GCU_Vector8 * vector, GCU_Type8_Union value);

/**
 * Get a count of entries in the vector.
 *
 * @param vector The vector structure on which to operate.
 * @return The count of entries in the vector.
 */
GCU_API size_t gcu_vector8_count(GCU_Vector8 * vector);

/**
 * Reserve space in the vector.
 *
 * If the vector is larger than the requested amount, then nothing will be
 * done.  If the vector is smaller than the requested amount, then the vector
 * capacity will be increased to the requested amount (if possible).
 *
 * @param vector The vector structure on which to operate.
 * @param count The number of items to reserve.
 * @return `true` on success, `false` otherwise.
 */
GCU_API bool gcu_vector8_reserve(GCU_Vector8 * vector, size_t count);

#ifdef __cplusplus
}
#endif

#endif //GHOTI_IO_GCU_VECTOR_H

