// Copyright (c) 2018 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include <windows.h>
#undef DeleteFile

#include <codecvt>
#include <unordered_set>

#include "leveldb/env.h"

#include "port/port.h"
#include "util/env_windows_test_helper.h"
#include "util/testharness.h"

namespace {

void GetOpenHandles(std::unordered_set<HANDLE>* open_handles) {
  constexpr int kHandleOffset = 4;
  const HANDLE kHandleUpperBound = reinterpret_cast<HANDLE>(1000 * kHandleOffset);

  for (HANDLE handle = nullptr; handle < kHandleUpperBound; reinterpret_cast<size_t&>(handle) += kHandleOffset) {
    DWORD dwFlags;
    if (!GetHandleInformation(handle, &dwFlags)) {
      ASSERT_EQ(ERROR_INVALID_HANDLE, ::GetLastError())
          ;//<< "GetHandleInformation() should return ERROR_INVALID_HANDLE error on invalid handles";
      continue;
    }
    open_handles->insert(handle);
  }
}

void GetOpenedFileHandleByFileName(const char* name, HANDLE* result_handle) {
  std::unordered_set<HANDLE> open_handles;
  GetOpenHandles(&open_handles);

  for (HANDLE handle : open_handles) {
    char handle_path[MAX_PATH];
    DWORD ret = ::GetFinalPathNameByHandleA(handle, handle_path, sizeof handle_path, FILE_NAME_NORMALIZED);
    if (ret == 0) {
      continue;
    }

    ASSERT_GT(sizeof handle_path, ret);// << "Path too long";
    const char* last_backslash = std::strrchr(handle_path, '\\');
    ASSERT_NE(last_backslash, static_cast<const char*>(nullptr));
    if (std::strcmp(name, last_backslash + 1) == 0) {
      *result_handle = handle;
      return;
    }
  }

  ASSERT_TRUE(false);// << "File handle not found";
}

void CheckOpenedFileHandleNonInheritable(const char* name) {
  HANDLE handle = INVALID_HANDLE_VALUE;
  GetOpenedFileHandleByFileName(name, &handle);

  DWORD dwFlags;
  ASSERT_TRUE(GetHandleInformation(handle, &dwFlags));
  ASSERT_TRUE(!(dwFlags & HANDLE_FLAG_INHERIT));
}

}  // namespace

namespace leveldb {

static const int kMMapLimit = 4;

class EnvWindowsTest {
 public:
  static void SetFileLimits(int mmap_limit) {
    EnvWindowsTestHelper::SetReadOnlyMMapLimit(mmap_limit);
  }

  EnvWindowsTest() : env_(Env::Default()) {}

  Env* env_;
};

TEST(EnvWindowsTest, TestOpenOnRead) {
  // Write some test data to a single file that will be opened |n| times.
  std::string test_dir;
  ASSERT_OK(env_->GetTestDirectory(&test_dir));
  std::string test_file = test_dir + "/open_on_read.txt";

  FILE* f = fopen(test_file.c_str(), "wN");
  ASSERT_TRUE(f != nullptr);
  const char kFileData[] = "abcdefghijklmnopqrstuvwxyz";
  fputs(kFileData, f);
  fclose(f);

  // Open test file some number above the sum of the two limits to force
  // leveldb::WindowsEnv to switch from mapping the file into memory
  // to basic file reading.
  const int kNumFiles = kMMapLimit + 5;
  leveldb::RandomAccessFile* files[kNumFiles] = {0};
  for (int i = 0; i < kNumFiles; i++) {
    ASSERT_OK(env_->NewRandomAccessFile(test_file, &files[i]));
  }
  char scratch;
  Slice read_result;
  for (int i = 0; i < kNumFiles; i++) {
    ASSERT_OK(files[i]->Read(i, 1, &read_result, &scratch));
    ASSERT_EQ(kFileData[i], read_result[0]);
  }
  for (int i = 0; i < kNumFiles; i++) {
    delete files[i];
  }
  ASSERT_OK(env_->DeleteFile(test_file));
}

TEST(EnvWindowsTest, TestHandleNotInheritedLogger) {
  std::string test_dir;
  ASSERT_OK(env_->GetTestDirectory(&test_dir));
  const char kFileName[] = "handle_not_inherited_logger.txt";
  std::string file_path = test_dir + "/" + kFileName;
  ASSERT_OK(WriteStringToFile(env_, "0123456789", file_path));

  leveldb::Logger* file = nullptr;
  ASSERT_OK(env_->NewLogger(file_path, &file));
  CheckOpenedFileHandleNonInheritable(kFileName);
  delete file;

  ASSERT_OK(env_->DeleteFile(file_path));
}

TEST(EnvWindowsTest, TestOpenOnRead_Unicode) {
  // Write some test data to a single file that will be opened |n| times.
  std::string test_dir;
  ASSERT_OK(env_->GetTestDirectory(&test_dir));
  std::string test_file = test_dir + u8"/open_on_runðŸƒ_read.txt";

  std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> converter;
  std::wstring wideUtf8Path = converter.from_bytes(test_file);
  FILE* f = _wfopen(wideUtf8Path.c_str(), L"w");
  ASSERT_TRUE(f != nullptr);
  const char kFileData[] = "abcdefghijklmnopqrstuvwxyz";
  fputs(kFileData, f);
  fclose(f);

  // Open test file some number above the sum of the two limits to force
  // leveldb::WindowsEnv to switch from mapping the file into memory
  // to basic file reading.
  const int kNumFiles = kMMapLimit + 5;
  leveldb::RandomAccessFile* files[kNumFiles] = {0};
  for (int i = 0; i < kNumFiles; i++) {
    ASSERT_OK(env_->NewRandomAccessFile(test_file, &files[i]));
  }
  char scratch;
  Slice read_result;
  for (int i = 0; i < kNumFiles; i++) {
    ASSERT_OK(files[i]->Read(i, 1, &read_result, &scratch));
    ASSERT_EQ(kFileData[i], read_result[0]);
  }
  for (int i = 0; i < kNumFiles; i++) {
    delete files[i];
  }
  ASSERT_OK(env_->DeleteFile(test_file));
}

TEST(EnvWindowsTest, TestGetChildrenEmpty) {
  // Create some dummy files.
  std::string test_dir;
  ASSERT_OK(env_->GetTestDirectory(&test_dir));

  std::vector<std::string> result;
  ASSERT_OK(env_->GetChildren(test_dir, &result));
  ASSERT_EQ(2, result.size()); // "." and ".." are always returned.
}

TEST(EnvWindowsTest, TestGetChildren_ChildFiles) {
  // Create some dummy files.
  std::string test_dir;
  ASSERT_OK(env_->GetTestDirectory(&test_dir));

  int childFilesCount = 10;
  for (int i = 0; i < childFilesCount; i++) {
    std::string test_file = test_dir + u8"/runðŸƒ_and_jumpðŸ¦˜_" + std::to_string(i) + ".txt";
    std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> converter;
    std::wstring wTest_file = converter.from_bytes(test_file);
    FILE* f = _wfopen(wTest_file.c_str(), L"w");
    ASSERT_TRUE(f != nullptr);
    fclose(f);
  }

  std::vector<std::string> result;
  ASSERT_OK(env_->GetChildren(test_dir, &result));
  ASSERT_EQ(childFilesCount + 2, result.size()); // "." and ".." are returned.
}

}  // namespace leveldb

int main(int argc, char** argv) {
  // All tests currently run with the same read-only file limits.
  leveldb::EnvWindowsTest::SetFileLimits(leveldb::kMMapLimit);
  return leveldb::test::RunAllTests();
}
