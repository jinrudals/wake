/*
 * Copyright 2019 SiFive, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You should have received a copy of LICENSE.Apache2 along with
 * this software. If not, you may obtain a copy at
 *
 *    https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef FILE_H
#define FILE_H

#include <stdint.h>

#include <string>
#include <vector>

#include "location.h"
#include "segment.h"

class DiagnosticReporter;

class FileContent {
 public:
  FileContent(const char *filename_);
  FileContent(FileContent &&o);
  FileContent &operator=(FileContent &&o);

  // position points to any byte of the codepoint
  Coordinates coordinates(const uint8_t *position) const;

  void clearNewLines();
  void addNewline(const uint8_t *first_column);

  StringSegment segment() const { return ss; }
  const char *filename() const { return fname.c_str(); }

  // copy construction is forbidden
  FileContent(const FileContent &) = delete;
  FileContent &operator=(const FileContent &) = delete;

 protected:
  StringSegment ss;
  std::string fname;
  std::vector<size_t> newlines;
};

class StringFile : public FileContent {
 public:
  StringFile(const char *filename_, std::string &&content_);
  StringFile(StringFile &&o) = default;
  StringFile &operator=(StringFile &&o) = default;

 private:
  std::string content;
};

class ExternalFile : public FileContent {
 public:
  ExternalFile(DiagnosticReporter &reporter, const char *filename_, const char *uriScheme);
  ~ExternalFile();

  ExternalFile(ExternalFile &&o) = default;
  ExternalFile &operator=(ExternalFile &&o) = default;
};

class CPPFile : public FileContent {
 public:
  CPPFile(const char *filename);
};

#endif
