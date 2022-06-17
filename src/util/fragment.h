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

#ifndef FRAGMENT_H
#define FRAGMENT_H

#include "file.h"
#include "segment.h"

class FileFragment {
 public:
  FileFragment(const FileContent *content_, uint32_t start_, uint32_t end_)
      : content(content_), start(start_), end(end_) {}
  FileFragment(const FileContent *content_, StringSegment ss)
      : content(content_),
        start(ss.start - content->segment().start),
        end(ss.end - content->segment().start) {}

  Location location() const;
  StringSegment segment() const {
    return StringSegment{content->segment().start + start, content->segment().start + end};
  }

  const FileContent *fcontent() const { return content; }
  const char *filename() const { return content->filename(); }

  uint32_t startByte() const { return start; }
  uint32_t endByte() const { return end; }

  bool empty() const { return start == end; }

 private:
  const FileContent *content;
  uint32_t start, end;
};

#define FRAGMENT_CPP_LINE FileFragment(&cppFile, __LINE__, __LINE__)

#endif
