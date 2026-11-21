#pragma once
// Lessons in a form every application can use: what a fix did, not whose code it was done in.
//
// A lesson record holds the lines before ("removed") and after ("added") a fix, as one string or as a list of lines. The
// program's own names that the fix keeps on both sides (variables, members, its own functions: m_pImpl, sParam, joined)
// become name1, name2, ... and a string literal the fix keeps unchanged becomes "text1", "text2", ... - the same
// placeholder for the same name or text in the before and after lines and in the message. Comments go. What the fix is
// about stays: a name on one side only (CharUpperBuffA -> CharUpperBuff, DWORD -> DWORD_PTR), the names of the Windows
// API, the C and C++ libraries, MFC and ATL, keywords, macros and constants (ALL_CAPS), and format strings ("%d" -> "%zu").
// An application that replays lessons treats the placeholders as wildcards: a fix kept for `int len = strlen(s)` fits
// `int n = strlen(name)`.
#include <nlohmann/json.hpp>

namespace modelhub
{

// The record in its general form, marked "general": true; one already marked comes back as it is.
nlohmann::json GeneralLesson(const nlohmann::json& record);

// The number of lines on the longer side of a record (its "removed" or "added").
size_t LessonLines(const nlohmann::json& record);

}  // namespace modelhub
