/*
Copyright (c) 2015 - 2021 Advanced Micro Devices, Inc. All rights reserved.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/
#ifndef HIPRTC_H
#define HIPRTC_H

/**
 *  @addtogroup Runtime Runtime Compilation
 *  @{
 *  @ingroup Runtime
 */

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

#include <stdlib.h>

#if !defined(_WIN32)
#pragma GCC visibility push (default)
#endif

/**
 * @brief hiprtcResult
 * @enum
 *
 */

typedef enum hiprtcResult {
    HIPRTC_SUCCESS = 0,
    HIPRTC_ERROR_OUT_OF_MEMORY = 1,
    HIPRTC_ERROR_PROGRAM_CREATION_FAILURE = 2,
    HIPRTC_ERROR_INVALID_INPUT = 3,
    HIPRTC_ERROR_INVALID_PROGRAM = 4,
    HIPRTC_ERROR_INVALID_OPTION = 5,
    HIPRTC_ERROR_COMPILATION = 6,
    HIPRTC_ERROR_BUILTIN_OPERATION_FAILURE = 7,
    HIPRTC_ERROR_NO_NAME_EXPRESSIONS_AFTER_COMPILATION = 8,
    HIPRTC_ERROR_NO_LOWERED_NAMES_BEFORE_COMPILATION = 9,
    HIPRTC_ERROR_NAME_EXPRESSION_NOT_VALID = 10,
    HIPRTC_ERROR_INTERNAL_ERROR = 11
} hiprtcResult;

 /**
 * @brief Returns text string message to explain the error which occurred
 *
 * @param [in] result  code to convert to string.
 * @return  const char pointer to the NULL-terminated error string
 *
 * @warning In HIP, this function returns the name of the error,
 * if the hiprtc result is defined, it will return "Invalid HIPRTC error code"
 *
 * @see hiprtcResult
 */
const char* hiprtcGetErrorString(hiprtcResult result);

/**
 * @brief Sets the parameters as major and minor version.
 *
 * @param [out] major  HIP Runtime Compilation major version.
 * @param [out] minor  HIP Runtime Compilation minor version.
 *
 */
hiprtcResult hiprtcVersion(int* major, int* minor);

typedef struct _hiprtcProgram* hiprtcProgram;

/**
 * @brief Adds the given name exprssion to the runtime compilation program.
 *
 * @param [in] prog  runtime compilation program instance.
 * @param [in] name_expression  const char pointer to the name expression.
 * @return  HIPRTC_SUCCESS
 *
 * If const char pointer is NULL, it will return HIPRTC_ERROR_INVALID_INPUT.
 *
 * @see hiprtcResult
 */
hiprtcResult hiprtcAddNameExpression(hiprtcProgram prog,
                                     const char* name_expression);

/**
 * @brief Compiles the given runtime compilation program.
 *
 * @param [in] prog  runtime compilation program instance.
 * @param [in] numOptions  number of compiler options.
 * @param [in] options  compiler options as const array of strins.
 * @return HIPRTC_SUCCESS
 *
 * If the compiler failed to build the runtime compilation program,
 * it will return HIPRTC_ERROR_COMPILATION.
 *
 * @see hiprtcResult
 */
hiprtcResult hiprtcCompileProgram(hiprtcProgram prog,
                                  int numOptions,
                                  const char** options);

/**
 * @brief Creates an instance of hiprtcProgram with the given input parameters,
 * and sets the output hiprtcProgram prog with it.
 *
 * @param [in, out] prog  runtime compilation program instance.
 * @param [in] src  const char pointer to the program source.
 * @param [in] name  const char pointer to the program name.
 * @param [in] numHeaders  number of headers.
 * @param [in] headers  array of strings pointing to headers.
 * @param [in] includeNames  array of strings pointing to names included in program source.
 * @return HIPRTC_SUCCESS
 *
 * Any invalide input parameter, it will return HIPRTC_ERROR_INVALID_INPUT
 * or HIPRTC_ERROR_INVALID_PROGRAM.
 *
 * If failed to create the program, it will return HIPRTC_ERROR_PROGRAM_CREATION_FAILURE.
 *
 * @see hiprtcResult
 */
hiprtcResult hiprtcCreateProgram(hiprtcProgram* prog,
                                 const char* src,
                                 const char* name,
                                 int numHeaders,
                                 const char** headers,
                                 const char** includeNames);

/**
 * @brief Destroys an instance of given hiprtcProgram.
 *
 * @param [in] prog  runtime compilation program instance.
 * @return HIPRTC_SUCCESS
 *
 * If prog is NULL, it will return HIPRTC_ERROR_INVALID_INPUT.
 *
 * @see hiprtcResult
 */
hiprtcResult hiprtcDestroyProgram(hiprtcProgram* prog);

/**
 * @brief Gets the lowered (mangled) name from an instance of hiprtcProgram with the given input parameters,
 * and sets the output lowered_name with it.
 *
 * @param [in] prog  runtime compilation program instance.
 * @param [in] name_expression  const char pointer to the name expression.
 * @param [in, out] lowered_name  const char array to the lowered (mangled) name.
 * @return HIPRTC_SUCCESS
 *
 * If any invalide nullptr input parameters, it will return HIPRTC_ERROR_INVALID_INPUT
 *
 * If name_expression is not found, it will return HIPRTC_ERROR_NAME_EXPRESSION_NOT_VALID
 *
 * If failed to get lowered_name from the program, it will return HIPRTC_ERROR_COMPILATION.
 *
 * @see hiprtcResult
 */
hiprtcResult hiprtcGetLoweredName(hiprtcProgram prog,
                                  const char* name_expression,
                                  const char** lowered_name);

/**
 * @brief Gets the log generated by the runtime compilation program instance.
 *
 * @param [in] prog  runtime compilation program instance.
 * @param [out] log  memory pointer to the generated log.
 * @return HIPRTC_SUCCESS
 *
 * @see hiprtcResult
 */
hiprtcResult hiprtcGetProgramLog(hiprtcProgram prog, char* log);

/**
 * @brief Gets the size of log generated by the runtime compilation program instance.
 *
 * @param [in] prog  runtime compilation program instance.
 * @param [out] logSizeRet  size of generated log.
 * @return HIPRTC_SUCCESS
 *
 * @see hiprtcResult
 */
hiprtcResult hiprtcGetProgramLogSize(hiprtcProgram prog,
                                     size_t* logSizeRet);

/**
 * @brief Gets the pointer of compilation binary by the runtime compilation program instance.
 *
 * @param [in] prog  runtime compilation program instance.
 * @param [out] code  char pointer to binary.
 * @return HIPRTC_SUCCESS
 *
 * @see hiprtcResult
 */
hiprtcResult hiprtcGetCode(hiprtcProgram prog, char* code);

/**
 * @brief Gets the size of compilation binary by the runtime compilation program instance.
 *
 * @param [in] prog  runtime compilation program instance.
 * @param [out] code  the size of binary.
 * @return HIPRTC_SUCCESS
 *
 * @see hiprtcResult
 */
hiprtcResult hiprtcGetCodeSize(hiprtcProgram prog, size_t* codeSizeRet);

#if !defined(_WIN32)
#pragma GCC visibility pop
#endif

#ifdef __cplusplus
}
#endif /* __cplusplus */
// doxygen end HIPrtc feature
/**
 * @}
 */
#endif //HIPRTC_H
