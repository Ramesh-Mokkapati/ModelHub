# port-fix lessons

## C4244 - Compiler Warning (level 2) C4244
- C4244 occurs when converting from a larger numeric type (like double or float) to a smaller one (like int), potentially losing precision or data
- Common fix is to explicitly cast the value to the target type: `int result = (int)doubleValue;`
- Another approach is to use rounding functions like `round()` or `floor()` before conversion to make the intent clear
- When porting, check all function calls where floating-point arguments are passed to integer parameters
- Use `/W3` or `/W4` compiler flags to catch these warnings early in the porting process
- Consider using static_assert or compile-time checks to validate ranges if the conversion must be safe

Source: https://learn.microsoft.com/en-us/cpp/error-messages/compiler-warnings/compiler-warning-level-2-c4244

_(PortAssistAI, 2026-11-18)_

## C2664 - Compiler Error C2664
- C2664 occurs when a function argument cannot be converted from one type to another, often due to missing explicit conversions or const reference issues
- When porting from older Visual C++ versions, wchar_t became a true built-in type and is no longer implicitly convertible to unsigned short - add explicit casts or use /Zc:wchar_t to control this behavior
- Function overloading issues arise when derived classes hide base class methods - use 'using Base::method' to expose inherited functions
- String conversion problems frequently occur when passing string literals to functions expecting specific string types - change parameter to const reference or add explicit conversion
- Enum class values need explicit casting to their underlying types when calling functions expecting those types directly
- Common issue with COM interfaces where wchar_t parameters don't match expected types in type libraries - cast parameters or modify IDL definitions accordingly

Source: https://learn.microsoft.com/en-us/cpp/error-messages/compiler-errors-2/compiler-error-c2664

_(PortAssistAI, 2026-11-18)_

## C2665 - Compiler Error C2665
- C2665 occurs when a function call cannot find an exact match for the parameters provided, and no implicit conversion exists between the argument types and the function parameter types
- Common when porting Windows code that uses narrow character strings (char) with Unicode functions expecting wide character strings (wchar_t), or vice versa
- Fix by explicitly casting parameters to match expected types, such as using (wchar_t*) or (char*) to force the correct overload
- For Unicode porting, use _T() macro or TEXT() macro to handle both ANSI and Unicode builds, or use wide string literals like L"string" instead of "string"
- When mixing narrow and wide character functions, ensure consistent use of char vs wchar_t types throughout the call chain, or use conversion functions like MultiByteToWideChar() or WideCharToMultiByte()

Source: https://learn.microsoft.com/en-us/cpp/error-messages/compiler-errors-2/compiler-error-c2665

_(PortAssistAI, 2026-11-18)_

## C2679 - Compiler Error C2679
- C2679 occurs when trying to use an operator (like =) with types that don't support it directly
- The error happens because the left operand type doesn't have an appropriate operator overload or conversion for the right operand type
- Common fix: Add a constructor to the class that accepts the right operand type (e.g., add `C(int)` to make `c = 10` work)
- Alternative fix: Implement the missing operator overload in the class (e.g., `operator=(int)` in class C)
- When porting, check for implicit conversions and ensure classes can accept the types being assigned to them
- For Unicode porting specifically, watch for string literal assignments like `wstring = L"literal"` - ensure proper constructors exist or use explicit conversion

Source: https://learn.microsoft.com/en-us/cpp/error-messages/compiler-errors-2/compiler-error-c2679

_(PortAssistAI, 2026-11-18)_

## C2440 - Compiler Error C2440
- C2440 occurs when the compiler cannot convert between incompatible types, often due to const-correctness issues like assigning string literals to non-const char* pointers
- Fix by using const char* instead of char* for string literals, or enable /Zc:strictStrings to match C++ behavior
- When porting to C++20, u8 string literals are const char8_t and require reinterpret_cast to convert to const char*
- Pointer-to-member conversions to void* fail - use proper casting or avoid the conversion entirely
- Forward-declared types cause C2440 when casting - define the type before using it in casts
- Assigning zero to interior pointers fails - use nullptr instead of 0 for managed pointers

Source: https://learn.microsoft.com/en-us/cpp/error-messages/compiler-errors-1/compiler-error-c2440

_(PortAssistAI, 2026-11-19)_
