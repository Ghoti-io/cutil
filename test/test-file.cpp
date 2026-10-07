/**
 * @file
 *
 * Tests for the file module.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <gtest/gtest.h>
#include <ghoti.io/cutil/file.h>
#include <ghoti.io/cutil/path.h>

#ifdef _WIN32
#include <windows.h>
#include <ghoti.io/cutil/dir.h>
#else
#include <dirent.h>
#include <stdlib.h>
#include <unistd.h>
#endif

using namespace std;

namespace {

/** A scratch directory that removes itself and anything left in it. */
class Scratch : public testing::Test {
protected:
  string dir;

  void SetUp() override {
#ifdef _WIN32
    // No mkdtemp() here. gcu_dir_temp_create() is the dir module's, not the
    // one under test, and test-dir covers it.
    char * made = nullptr;
    ASSERT_EQ(GCU_FILE_OK,
        gcu_dir_temp_create(nullptr, "gcu-file-test", nullptr, &made));
    dir = made;
    gcu_dir_free_path(nullptr, made);
#else
    char * temp_root = nullptr;
    ASSERT_EQ(GCU_PATH_OK, gcu_path_temp_dir(nullptr, &temp_root));
    string tmpl = string(temp_root) + "/gcu-file-test-XXXXXX";
    gcu_path_free(nullptr, temp_root);
    vector<char> buffer(tmpl.begin(), tmpl.end());
    buffer.push_back('\0');
    ASSERT_NE(nullptr, mkdtemp(buffer.data()));
    dir = buffer.data();
#endif
  }

  void TearDown() override {
    if (dir.empty()) {
      return;
    }
#ifdef _WIN32
    for (const string & name : entries()) {
      string path = dir + "/" + name;
      SetFileAttributesA(path.c_str(), FILE_ATTRIBUTE_NORMAL);
      DeleteFileA(path.c_str());
    }
    RemoveDirectoryA(dir.c_str());
#else
    if (DIR * d = opendir(dir.c_str())) {
      while (struct dirent * e = readdir(d)) {
        string name = e->d_name;
        if (name != "." && name != "..") {
          remove((dir + "/" + name).c_str());
        }
      }
      closedir(d);
    }
    rmdir(dir.c_str());
#endif
  }

  string at(const string & name) const { return dir + "/" + name; }

  /** Everything in the scratch directory, sorted. */
  vector<string> entries() const {
    vector<string> found;
#ifdef _WIN32
    WIN32_FIND_DATAA e;
    HANDLE d = FindFirstFileA((dir + "/*").c_str(), &e);
    if (d != INVALID_HANDLE_VALUE) {
      do {
        string name = e.cFileName;
        if (name != "." && name != "..") {
          found.push_back(name);
        }
      } while (FindNextFileA(d, &e));
      FindClose(d);
    }
#else
    if (DIR * d = opendir(dir.c_str())) {
      while (struct dirent * e = readdir(d)) {
        string name = e->d_name;
        if (name != "." && name != "..") {
          found.push_back(name);
        }
      }
      closedir(d);
    }
#endif
    sort(found.begin(), found.end());
    return found;
  }
};

void put(const string & path, const string & bytes) {
  FILE * f = fopen(path.c_str(), "wb");
  ASSERT_NE(nullptr, f);
  if (!bytes.empty()) {
    ASSERT_EQ(bytes.size(), fwrite(bytes.data(), 1, bytes.size(), f));
  }
  ASSERT_EQ(0, fclose(f));
}

/** Read through the module under test, returning content and length. */
struct Read {
  void * data = nullptr;
  size_t len = 0;
  GCU_File_Result result = GCU_FILE_OK;

  explicit Read(const string & path, size_t max = GCU_FILE_UNLIMITED) {
    result = gcu_file_read(path.c_str(), max, nullptr, &data, &len);
  }
  ~Read() { gcu_file_free(nullptr, data); }
  string str() const {
    return data ? string((const char *)data, len) : string();
  }
  const char * chars() const { return (const char *)data; }
};

bool exists(const string & path) {
  struct stat info;
  return stat(path.c_str(), &info) == 0;
}

} // namespace

//////////////////////////////////////////////////////////////////////////////
// Reading
//////////////////////////////////////////////////////////////////////////////

TEST_F(Scratch, ReadReturnsEveryByteAndAddsATerminatorPastTheEnd) {
  put(at("f"), "hello world");
  Read r(at("f"));
  ASSERT_EQ(GCU_FILE_OK, r.result);
  EXPECT_EQ(11u, r.len);
  EXPECT_EQ("hello world", r.str());
  // The terminator is past the length, so a text caller may use the buffer as
  // a C string and a binary caller can ignore it.
  EXPECT_EQ('\0', r.chars()[r.len]);
  EXPECT_STREQ("hello world", r.chars());
}

TEST_F(Scratch, ReadOfAnEmptyFileSucceedsWithZeroLength) {
  put(at("empty"), "");
  Read r(at("empty"));
  ASSERT_EQ(GCU_FILE_OK, r.result);
  EXPECT_EQ(0u, r.len);
  ASSERT_NE(nullptr, r.data) << "a zero-length read must still give a buffer";
  EXPECT_EQ('\0', r.chars()[0]);
}

TEST_F(Scratch, ReadKeepsEmbeddedNulBytes) {
  // The added terminator must not be mistaken for the end of the content.
  string binary("a\0b\0\0c", 6);
  put(at("bin"), binary);
  Read r(at("bin"));
  ASSERT_EQ(GCU_FILE_OK, r.result);
  EXPECT_EQ(6u, r.len);
  EXPECT_EQ(binary, r.str());
}

TEST_F(Scratch, ReadCrossesTheChunkBoundaryItGrowsAt) {
  // Larger than one read, so the growth path runs rather than being skipped.
  string big;
  for (int i = 0; i < 40000; ++i) {
    big.push_back((char)('a' + (i % 26)));
  }
  put(at("big"), big);
  Read r(at("big"));
  ASSERT_EQ(GCU_FILE_OK, r.result);
  EXPECT_EQ(big.size(), r.len);
  EXPECT_EQ(big, r.str());
}

TEST(FileRead, WorksOnAFileThatReportsNoSize) {
#ifdef __linux__
  // Everything under /proc reports a size of zero.  An implementation that
  // seeks to the end to size the file first comes back with nothing at all,
  // which is why this one reads in chunks instead.
  Read r("/proc/version");
  ASSERT_EQ(GCU_FILE_OK, r.result);
  EXPECT_GT(r.len, 0u) << "read nothing from a file that reports no size";
  EXPECT_NE(string::npos, r.str().find("Linux"));
#endif
}

TEST_F(Scratch, ReadOfAMissingFileFailsAndAllocatesNothing) {
  void * data = (void *)0x1;
  size_t len = 99;
  // NOT_FOUND rather than IO since the read learned to tell them apart.
  EXPECT_EQ(GCU_FILE_ERR_NOT_FOUND,
      gcu_file_read(at("nope").c_str(), GCU_FILE_UNLIMITED, nullptr, &data,
          &len));
  EXPECT_EQ((void *)0x1, data) << "outputs must be untouched on failure";
  EXPECT_EQ(99u, len);
}

TEST_F(Scratch, ReadRefusesAFileOverTheLimitRatherThanTruncating) {
  put(at("f"), "0123456789");
  void * data = nullptr;
  size_t len = 0;
  // A limit is a promise.  Handing back the first five bytes would be a
  // different file that still looks like a file.
  EXPECT_EQ(GCU_FILE_ERR_LIMIT,
      gcu_file_read(at("f").c_str(), 5, nullptr, &data, &len));
  EXPECT_EQ(nullptr, data);
}

TEST_F(Scratch, ReadAcceptsAFileExactlyAtTheLimit) {
  put(at("f"), "0123456789");
  Read exact(at("f"), 10);
  EXPECT_EQ(GCU_FILE_OK, exact.result);
  EXPECT_EQ(10u, exact.len);

  // One byte under is one byte too many.
  Read over(at("f"), 9);
  EXPECT_EQ(GCU_FILE_ERR_LIMIT, over.result);

  // And an empty file passes a limit of zero bytes... which means unlimited.
  Read unlimited(at("f"), GCU_FILE_UNLIMITED);
  EXPECT_EQ(GCU_FILE_OK, unlimited.result);
  EXPECT_EQ(10u, unlimited.len);
}

TEST_F(Scratch, ReadOfADirectoryIsAnError) {
  void * data = nullptr;
  size_t len = 0;
  EXPECT_NE(GCU_FILE_OK,
      gcu_file_read(dir.c_str(), GCU_FILE_UNLIMITED, nullptr, &data, &len));
  EXPECT_EQ(nullptr, data);
}

//////////////////////////////////////////////////////////////////////////////
// Temporary files
//////////////////////////////////////////////////////////////////////////////

TEST_F(Scratch, TempCreateOpensAFileInTheDirectoryItWasGiven) {
  GCU_File_Temp temp;
  ASSERT_EQ(GCU_FILE_OK,
      gcu_file_temp_create(&temp, dir.c_str(), "pre", nullptr));
  string path = gcu_file_temp_path(&temp);
  EXPECT_NE(nullptr, gcu_file_temp_stream(&temp));
  // Being in the destination's directory is what makes the later rename
  // atomic, so it is a property worth asserting rather than assuming.
  EXPECT_EQ(dir, path.substr(0, dir.size()));
  // Joined natively, so the separator before the prefix is a backslash on
  // Windows.
  EXPECT_NE(string::npos,
      path.find(string(1, gcu_path_separator(GCU_PATH_NATIVE)) + "pre"));
  EXPECT_TRUE(exists(path));
  gcu_file_temp_abort(&temp);
}

TEST_F(Scratch, ManyLiveTemporariesWithOnePrefixAllGetTheirOwnName) {
  // mkstemp() draws six random characters and retries on a clash.  The
  // Windows arm once used _wmktemp_s, which fills the same six places with a
  // letter and the process id: 26 names per prefix per process, predictable,
  // and a failure after that.  Sixty at once is past that limit.
  const int kCount = 60;
  vector<GCU_File_Temp> temps(kCount);
  vector<string> names;
  for (int i = 0; i < kCount; ++i) {
    ASSERT_EQ(GCU_FILE_OK,
        gcu_file_temp_create(&temps[i], dir.c_str(), "many", nullptr))
        << "temporary " << i;
    names.push_back(gcu_file_temp_path(&temps[i]));
  }
  vector<string> sorted = names;
  sort(sorted.begin(), sorted.end());
  EXPECT_EQ(sorted.end(), adjacent_find(sorted.begin(), sorted.end()))
      << "two live temporaries were given the same name";
  for (auto & temp : temps) {
    gcu_file_temp_abort(&temp);
  }
}

TEST_F(Scratch, AnOpenTemporaryCanBeReplacedAtomically) {
  // POSIX lets a file be renamed over while something holds it open, and
  // callers rely on it: writing the final result atomically to a path they
  // made with gcu_file_temp_create() and have not closed.  Windows refuses
  // unless the open file shares delete access, which the temporary did not.
  GCU_File_Temp temp;
  ASSERT_EQ(GCU_FILE_OK,
      gcu_file_temp_create(&temp, dir.c_str(), "held", nullptr));
  string path = gcu_file_temp_path(&temp);

  const string replacement = "replaced while open";
  EXPECT_EQ(GCU_FILE_OK,
      gcu_file_write_atomic(path.c_str(), replacement.data(),
          replacement.size(), GCU_FILE_SYNC_NONE, GCU_FILE_PERMS_DEFAULT,
          nullptr));
  Read r(path);
  EXPECT_EQ(GCU_FILE_OK, r.result);
  EXPECT_EQ(replacement, r.str());
  gcu_file_temp_abort(&temp);
}

TEST_F(Scratch, TempCreateOpensAFileOnlyItsOwnerCanRead) {
#ifndef _WIN32
  GCU_File_Temp temp;
  ASSERT_EQ(GCU_FILE_OK,
      gcu_file_temp_create(&temp, dir.c_str(), nullptr, nullptr));
  struct stat info;
  ASSERT_EQ(0, stat(gcu_file_temp_path(&temp), &info));
  // A temporary file that anyone can read is a way to leak whatever is being
  // written through it.
  EXPECT_EQ(0, info.st_mode & (S_IRWXG | S_IRWXO))
      << "mode was " << oct << (info.st_mode & 07777);
  gcu_file_temp_abort(&temp);
#endif
}

TEST_F(Scratch, TempCreateProducesADifferentNameEachTime) {
  GCU_File_Temp a;
  GCU_File_Temp b;
  ASSERT_EQ(GCU_FILE_OK, gcu_file_temp_create(&a, dir.c_str(), "x", nullptr));
  ASSERT_EQ(GCU_FILE_OK, gcu_file_temp_create(&b, dir.c_str(), "x", nullptr));
  EXPECT_STRNE(gcu_file_temp_path(&a), gcu_file_temp_path(&b));
  gcu_file_temp_abort(&a);
  gcu_file_temp_abort(&b);
}

TEST_F(Scratch, TempCreateWithoutADirectoryUsesTheSystemTemporaryDirectory) {
  char * expected = nullptr;
  ASSERT_EQ(GCU_PATH_OK, gcu_path_temp_dir(nullptr, &expected));
  GCU_File_Temp temp;
  ASSERT_EQ(GCU_FILE_OK,
      gcu_file_temp_create(&temp, nullptr, "scratch", nullptr));
  string path = gcu_file_temp_path(&temp);
  EXPECT_EQ(string(expected), path.substr(0, strlen(expected)));
  gcu_path_free(nullptr, expected);
  gcu_file_temp_abort(&temp);
}

TEST_F(Scratch, TempAbortRemovesTheFileAndEmptiesTheHandle) {
  GCU_File_Temp temp;
  ASSERT_EQ(GCU_FILE_OK,
      gcu_file_temp_create(&temp, dir.c_str(), "t", nullptr));
  string path = gcu_file_temp_path(&temp);
  ASSERT_TRUE(exists(path));

  gcu_file_temp_abort(&temp);
  EXPECT_FALSE(exists(path)) << "abort left the temporary file behind";
  EXPECT_EQ(nullptr, gcu_file_temp_path(&temp));
  EXPECT_EQ(nullptr, gcu_file_temp_stream(&temp));
  EXPECT_TRUE(entries().empty());
}

TEST_F(Scratch, TempAbortIsSafeOnAHandleThatWasNeverOpenedOrIsAlreadySpent) {
  // This is the property that lets a caller put abort on an unconditional
  // cleanup path without tracking whether commit already ran - which is how
  // the hand-written copies this replaces came to forget the cleanup on one
  // branch and not the other.
  gcu_file_temp_abort(nullptr);

  GCU_File_Temp zeroed;
  memset(&zeroed, 0, sizeof zeroed);
  gcu_file_temp_abort(&zeroed);

  GCU_File_Temp failed;
  EXPECT_NE(GCU_FILE_OK,
      gcu_file_temp_create(&failed, "/no/such/directory", "t", nullptr));
  gcu_file_temp_abort(&failed);

  GCU_File_Temp twice;
  ASSERT_EQ(GCU_FILE_OK,
      gcu_file_temp_create(&twice, dir.c_str(), "t", nullptr));
  gcu_file_temp_abort(&twice);
  gcu_file_temp_abort(&twice);

  GCU_File_Temp committed;
  ASSERT_EQ(GCU_FILE_OK,
      gcu_file_temp_create(&committed, dir.c_str(), "t", nullptr));
  ASSERT_EQ(GCU_FILE_OK,
      gcu_file_temp_commit(&committed, at("done").c_str(),
          GCU_FILE_SYNC_FULL, GCU_FILE_PERMS_PRIVATE));
  gcu_file_temp_abort(&committed);
  EXPECT_TRUE(exists(at("done"))) << "abort after commit deleted the result";
}

TEST_F(Scratch, TempCommitMovesTheContentIntoPlaceAndLeavesNoTemporary) {
  GCU_File_Temp temp;
  ASSERT_EQ(GCU_FILE_OK,
      gcu_file_temp_create(&temp, dir.c_str(), "t", nullptr));
  string temp_path = gcu_file_temp_path(&temp);
  ASSERT_EQ(5u, fwrite("abcde", 1, 5, gcu_file_temp_stream(&temp)));

  ASSERT_EQ(GCU_FILE_OK,
      gcu_file_temp_commit(&temp, at("out").c_str(), GCU_FILE_SYNC_FULL, GCU_FILE_PERMS_PRIVATE));

  EXPECT_FALSE(exists(temp_path));
  Read r(at("out"));
  ASSERT_EQ(GCU_FILE_OK, r.result);
  EXPECT_EQ("abcde", r.str());
  EXPECT_EQ(vector<string>{"out"}, entries());
}

TEST_F(Scratch, TempCommitReplacesAFileThatAlreadyExists) {
  put(at("out"), "the old contents, which are longer");
  GCU_File_Temp temp;
  ASSERT_EQ(GCU_FILE_OK,
      gcu_file_temp_create(&temp, dir.c_str(), "t", nullptr));
  ASSERT_EQ(3u, fwrite("new", 1, 3, gcu_file_temp_stream(&temp)));
  ASSERT_EQ(GCU_FILE_OK,
      gcu_file_temp_commit(&temp, at("out").c_str(), GCU_FILE_SYNC_FULL, GCU_FILE_PERMS_PRIVATE));

  Read r(at("out"));
  ASSERT_EQ(GCU_FILE_OK, r.result);
  EXPECT_EQ("new", r.str()) << "the old content survived the replacement";
  EXPECT_EQ(vector<string>{"out"}, entries());
}

TEST_F(Scratch, CommitIsRefusedOnAHandleWithNothingOpen) {
  GCU_File_Temp zeroed;
  memset(&zeroed, 0, sizeof zeroed);
  EXPECT_EQ(GCU_FILE_ERR_INVALID,
      gcu_file_temp_commit(&zeroed, at("x").c_str(), GCU_FILE_SYNC_FULL, GCU_FILE_PERMS_PRIVATE));
  EXPECT_EQ(GCU_FILE_ERR_INVALID,
      gcu_file_temp_commit(nullptr, at("x").c_str(), GCU_FILE_SYNC_FULL, GCU_FILE_PERMS_PRIVATE));

  GCU_File_Temp temp;
  ASSERT_EQ(GCU_FILE_OK,
      gcu_file_temp_create(&temp, dir.c_str(), "t", nullptr));
  EXPECT_EQ(GCU_FILE_ERR_INVALID,
      gcu_file_temp_commit(&temp, nullptr, GCU_FILE_SYNC_FULL, GCU_FILE_PERMS_PRIVATE));
  gcu_file_temp_abort(&temp);
}

TEST_F(Scratch, CommitIntoAMissingDirectoryFailsAndRemovesTheTemporary) {
  // The temporary file is created successfully and it is the *rename* that
  // fails, which is the only path on which commit has litter of its own to
  // clean up.  Nothing else in this suite reaches it.
  GCU_File_Temp temp;
  ASSERT_EQ(GCU_FILE_OK,
      gcu_file_temp_create(&temp, dir.c_str(), "t", nullptr));
  string temp_path = gcu_file_temp_path(&temp);
  ASSERT_EQ(3u, fwrite("abc", 1, 3, gcu_file_temp_stream(&temp)));

  EXPECT_EQ(GCU_FILE_ERR_IO,
      gcu_file_temp_commit(&temp, (dir + "/no/such/dir/out").c_str(),
          GCU_FILE_SYNC_FULL, GCU_FILE_PERMS_PRIVATE));

  EXPECT_FALSE(exists(temp_path)) << "a failed commit left its temporary";
  EXPECT_TRUE(entries().empty());
  // Spent either way, so an unconditional abort afterwards is still safe.
  EXPECT_EQ(nullptr, gcu_file_temp_path(&temp));
  gcu_file_temp_abort(&temp);
}

//////////////////////////////////////////////////////////////////////////////
// write_atomic
//////////////////////////////////////////////////////////////////////////////

TEST_F(Scratch, WriteAtomicRoundTripsAndLeavesNothingBehind) {
  string payload("some\0bytes", 10);
  ASSERT_EQ(GCU_FILE_OK,
      gcu_file_write_atomic(at("out").c_str(), payload.data(), payload.size(),
          GCU_FILE_SYNC_FULL, GCU_FILE_PERMS_PRIVATE, nullptr));
  Read r(at("out"));
  ASSERT_EQ(GCU_FILE_OK, r.result);
  EXPECT_EQ(payload, r.str());
  // A temporary left in the directory is how the copy this replaces failed.
  EXPECT_EQ(vector<string>{"out"}, entries());
}

TEST_F(Scratch, WriteAtomicReplacesAnExistingFileAndAcceptsZeroLength) {
  put(at("out"), "previous");
  ASSERT_EQ(GCU_FILE_OK,
      gcu_file_write_atomic(at("out").c_str(), nullptr, 0,
          GCU_FILE_SYNC_FULL, GCU_FILE_PERMS_PRIVATE, nullptr));
  Read r(at("out"));
  ASSERT_EQ(GCU_FILE_OK, r.result);
  EXPECT_EQ(0u, r.len);
  EXPECT_EQ(vector<string>{"out"}, entries());
}

TEST_F(Scratch, WriteAtomicWritesTheContentWithoutSyncingToo) {
  // SYNC_NONE changes only the durability promise, never the content.
  ASSERT_EQ(GCU_FILE_OK,
      gcu_file_write_atomic(at("out").c_str(), "xyz", 3, GCU_FILE_SYNC_NONE, GCU_FILE_PERMS_PRIVATE,
          nullptr));
  Read r(at("out"));
  ASSERT_EQ(GCU_FILE_OK, r.result);
  EXPECT_EQ("xyz", r.str());
}

TEST_F(Scratch, WriteAtomicIntoAMissingDirectoryFailsAndLeavesNoLitter) {
  EXPECT_NE(GCU_FILE_OK,
      gcu_file_write_atomic((dir + "/no/such/dir/out").c_str(), "x", 1,
          GCU_FILE_SYNC_FULL, GCU_FILE_PERMS_PRIVATE, nullptr));
  EXPECT_TRUE(entries().empty());
}

TEST_F(Scratch, WriteAtomicDoesNotUseTheSystemTemporaryDirectory) {
#ifndef _WIN32
  // The temporary must be created beside the destination, because a rename
  // between filesystems is a copy and a copy is not atomic.  Pointing $TMPDIR
  // at nothing makes the difference observable: a version that reached for
  // the system temporary directory cannot create its file at all, while the
  // correct one never looks there.
  const char * saved = getenv("TMPDIR");
  string restore = saved ? saved : "";
  setenv("TMPDIR", "/no/such/temporary/directory", 1);

  GCU_File_Result result = gcu_file_write_atomic(at("out").c_str(), "abc", 3,
      GCU_FILE_SYNC_FULL, GCU_FILE_PERMS_PRIVATE, nullptr);

  if (saved) {
    setenv("TMPDIR", restore.c_str(), 1);
  }
  else {
    unsetenv("TMPDIR");
  }

  ASSERT_EQ(GCU_FILE_OK, result);
  Read r(at("out"));
  ASSERT_EQ(GCU_FILE_OK, r.result);
  EXPECT_EQ("abc", r.str());
  EXPECT_EQ(vector<string>{"out"}, entries());
#endif
}

TEST_F(Scratch, NullArgumentsAreRefusedRatherThanFatal) {
  void * data = nullptr;
  size_t len = 0;
  EXPECT_EQ(GCU_FILE_ERR_INVALID,
      gcu_file_read(nullptr, GCU_FILE_UNLIMITED, nullptr, &data, &len));
  EXPECT_EQ(GCU_FILE_ERR_INVALID,
      gcu_file_read(at("x").c_str(), GCU_FILE_UNLIMITED, nullptr, nullptr,
          &len));
  EXPECT_EQ(GCU_FILE_ERR_INVALID,
      gcu_file_read(at("x").c_str(), GCU_FILE_UNLIMITED, nullptr, &data,
          nullptr));
  EXPECT_EQ(GCU_FILE_ERR_INVALID,
      gcu_file_temp_create(nullptr, nullptr, nullptr, nullptr));
  EXPECT_EQ(GCU_FILE_ERR_INVALID,
      gcu_file_write_atomic(nullptr, "x", 1, GCU_FILE_SYNC_FULL, GCU_FILE_PERMS_PRIVATE, nullptr));
  // Bytes may only be NULL when there are none of them.
  EXPECT_EQ(GCU_FILE_ERR_INVALID,
      gcu_file_write_atomic(at("x").c_str(), nullptr, 5, GCU_FILE_SYNC_FULL, GCU_FILE_PERMS_PRIVATE,
          nullptr));
  EXPECT_EQ(nullptr, gcu_file_temp_stream(nullptr));
  EXPECT_EQ(nullptr, gcu_file_temp_path(nullptr));
  gcu_file_free(nullptr, nullptr);
}

#ifndef _WIN32
/** The permission bits of a path, or -1 if it is not there. */
int mode_of(const string & path) {
  struct stat info;
  if (stat(path.c_str(), &info) != 0) {
    return -1;
  }
  return (int)(info.st_mode & 07777);
}

/** What an ordinary fopen() in this directory produces, right now. */
int reference_mode(const string & dir) {
  string probe = dir + "/reference-probe";
  put(probe, "x");
  int mode = mode_of(probe);
  remove(probe.c_str());
  return mode;
}

using FilePerms = Scratch;

TEST_F(FilePerms, PrivateIsTheZeroValueAndKeepsTheFileToItsOwner) {
  // Asserted against the zero value spelled as 0, not as the enumerator,
  // because the guarantee being made is about what a caller who passes
  // nothing thoughtful gets.
  ASSERT_EQ(GCU_FILE_OK,
      gcu_file_write_atomic(at("secret").c_str(), "s", 1, GCU_FILE_SYNC_FULL,
          (GCU_File_Perms)0, nullptr));
  EXPECT_EQ(0600, mode_of(at("secret")));
}

TEST_F(FilePerms, DefaultMatchesWhatAnOrdinaryOpenWouldHaveProduced) {
  // The reference is measured rather than written down: it depends on the
  // umask this test happens to run under, and on any default ACL on the
  // temporary directory, neither of which the test may assume.
  int reference = reference_mode(dir);
  ASSERT_NE(-1, reference);
  ASSERT_EQ(GCU_FILE_OK,
      gcu_file_write_atomic(at("out").c_str(), "o", 1, GCU_FILE_SYNC_FULL,
          GCU_FILE_PERMS_DEFAULT, nullptr));
  EXPECT_EQ(reference, mode_of(at("out")));
}

TEST_F(FilePerms, DefaultFollowsAChangedUmaskRatherThanAFixedNumber) {
  // Two writes under two umasks. If the mode were a constant in the source,
  // or read once and cached, these would agree; they must not.
  mode_t saved = umask(0077);
  ASSERT_EQ(GCU_FILE_OK,
      gcu_file_write_atomic(at("tight").c_str(), "t", 1, GCU_FILE_SYNC_FULL,
          GCU_FILE_PERMS_DEFAULT, nullptr));
  int tight = mode_of(at("tight"));
  umask(0022);
  ASSERT_EQ(GCU_FILE_OK,
      gcu_file_write_atomic(at("loose").c_str(), "l", 1, GCU_FILE_SYNC_FULL,
          GCU_FILE_PERMS_DEFAULT, nullptr));
  int loose = mode_of(at("loose"));
  umask(saved);

  EXPECT_EQ(0600, tight);
  EXPECT_EQ(0644, loose);
}

TEST_F(FilePerms, PreserveKeepsTheModeTheDestinationAlreadyHad) {
  put(at("config"), "old");
  ASSERT_EQ(0, chmod(at("config").c_str(), 0640));

  ASSERT_EQ(GCU_FILE_OK,
      gcu_file_write_atomic(at("config").c_str(), "new", 3,
          GCU_FILE_SYNC_FULL, GCU_FILE_PERMS_PRESERVE, nullptr));

  EXPECT_EQ(0640, mode_of(at("config")));
  Read r(at("config"));
  ASSERT_EQ(GCU_FILE_OK, r.result);
  EXPECT_EQ("new", r.str());
}

TEST_F(FilePerms, PreserveDoesNotWidenAFileSomebodyNarrowedOnPurpose) {
  // The case that motivates the value existing: a umask that would have
  // produced 0644 must not undo a deliberate chmod 600.
  mode_t saved = umask(0022);
  put(at("key"), "old");
  ASSERT_EQ(0, chmod(at("key").c_str(), 0600));
  ASSERT_EQ(GCU_FILE_OK,
      gcu_file_write_atomic(at("key").c_str(), "new", 3, GCU_FILE_SYNC_FULL,
          GCU_FILE_PERMS_PRESERVE, nullptr));
  umask(saved);
  EXPECT_EQ(0600, mode_of(at("key")));
}

TEST_F(FilePerms, PreserveOnAFileThatIsNotThereYetCreatesItTheOrdinaryWay) {
  int reference = reference_mode(dir);
  ASSERT_NE(-1, reference);
  ASSERT_EQ(-1, mode_of(at("fresh")));
  ASSERT_EQ(GCU_FILE_OK,
      gcu_file_write_atomic(at("fresh").c_str(), "f", 1, GCU_FILE_SYNC_FULL,
          GCU_FILE_PERMS_PRESERVE, nullptr));
  EXPECT_EQ(reference, mode_of(at("fresh")));
}

TEST_F(FilePerms, TheTemporaryStaysPrivateEvenWhenTheResultWillNotBe) {
  // The window this closes: between create and commit the temporary holds
  // the whole content under a name in a directory others may read.
  mode_t saved = umask(0022);
  GCU_File_Temp temp;
  ASSERT_EQ(GCU_FILE_OK,
      gcu_file_temp_create(&temp, dir.c_str(), "wide", nullptr));
  ASSERT_EQ(0600, mode_of(gcu_file_temp_path(&temp)));
  fputs("content", gcu_file_temp_stream(&temp));
  // Still private with the content in it, not merely at the moment of
  // creation.
  EXPECT_EQ(0600, mode_of(gcu_file_temp_path(&temp)));
  ASSERT_EQ(GCU_FILE_OK,
      gcu_file_temp_commit(&temp, at("wide").c_str(), GCU_FILE_SYNC_FULL,
          GCU_FILE_PERMS_DEFAULT));
  umask(saved);
  EXPECT_EQ(0644, mode_of(at("wide")));
}

TEST_F(FilePerms, TheProbeLeavesNothingBehind) {
  ASSERT_EQ(GCU_FILE_OK,
      gcu_file_write_atomic(at("out").c_str(), "x", 1, GCU_FILE_SYNC_FULL,
          GCU_FILE_PERMS_DEFAULT, nullptr));
  EXPECT_EQ(vector<string>{"out"}, entries());
}

TEST_F(FilePerms, TheProbeRefusesANameSomebodyElseAlreadyHolds) {
  // White-box, deliberately: the probe is created beside the temporary file,
  // at its name plus one character.  Planting a symbolic link there is the
  // attack the probe's O_EXCL exists to refuse - without it the probe would
  // follow the link and truncate whatever it points at, with the permissions
  // of whoever is running this.
  //
  // It is also the only way the suite can reach "the permissions could not be
  // settled" in isolation.  Making the directory unwritable instead breaks
  // the rename as well, so the call fails either way and the assertion holds
  // for the wrong reason.
  put(at("victim"), "must not be touched");

  GCU_File_Temp temp;
  ASSERT_EQ(GCU_FILE_OK,
      gcu_file_temp_create(&temp, dir.c_str(), "t", nullptr));
  string blocker = string(gcu_file_temp_path(&temp)) + "p";
  ASSERT_EQ(0, symlink(at("victim").c_str(), blocker.c_str()));
  fputs("replacement", gcu_file_temp_stream(&temp));

  GCU_File_Result result = gcu_file_temp_commit(&temp, at("dest").c_str(),
      GCU_FILE_SYNC_FULL, GCU_FILE_PERMS_DEFAULT);
  remove(blocker.c_str());

  EXPECT_EQ(GCU_FILE_ERR_IO, result);
  // Nothing was renamed into place: permissions that could not be settled
  // mean a replacement that did not happen.
  EXPECT_FALSE(exists(at("dest")));
  Read v(at("victim"));
  ASSERT_EQ(GCU_FILE_OK, v.result);
  EXPECT_EQ("must not be touched", v.str());
}

TEST_F(FilePerms, AnUnknownValueIsRefusedAndWritesNothing) {
  EXPECT_NE(GCU_FILE_OK,
      gcu_file_write_atomic(at("out").c_str(), "x", 1, GCU_FILE_SYNC_FULL,
          (GCU_File_Perms)999, nullptr));
  EXPECT_FALSE(exists(at("out")));
  EXPECT_TRUE(entries().empty());
}
#endif

#ifndef _WIN32
using FileMeta = Scratch;

TEST_F(FileMeta, StatReportsTypeSizeAndAPlausibleTime) {
  put(at("f"), "twelve bytes");
  GCU_File_Info info;
  ASSERT_EQ(GCU_FILE_OK, gcu_file_stat(at("f").c_str(), &info));
  EXPECT_EQ(GCU_FILE_TYPE_REGULAR, info.type);
  EXPECT_EQ(12u, info.size);
  // Not compared against a constant: the assertion is that the epoch and the
  // scale are right, which a wrong one gets wrong by decades.  1600000000 is
  // 2020; a value in seconds rather than nanoseconds lands far below it.
  EXPECT_GT(info.mtime_ns, (int64_t)1600000000 * 1000000000);
  EXPECT_LT(info.mtime_ns, (int64_t)4000000000 * 1000000000);
}

TEST_F(FileMeta, StatTellsADirectoryFromAFile) {
  put(at("f"), "x");
  GCU_File_Info file_info, dir_info;
  ASSERT_EQ(GCU_FILE_OK, gcu_file_stat(at("f").c_str(), &file_info));
  ASSERT_EQ(GCU_FILE_OK, gcu_file_stat(dir.c_str(), &dir_info));
  EXPECT_EQ(GCU_FILE_TYPE_REGULAR, file_info.type);
  EXPECT_EQ(GCU_FILE_TYPE_DIRECTORY, dir_info.type);
  EXPECT_TRUE(gcu_file_is_directory(dir.c_str()));
  EXPECT_FALSE(gcu_file_is_directory(at("f").c_str()));
}

TEST_F(FileMeta, StatFollowsALinkAndStatLinkDoesNot) {
  put(at("target"), "content");
  ASSERT_EQ(0, symlink(at("target").c_str(), at("link").c_str()));

  GCU_File_Info followed, itself;
  ASSERT_EQ(GCU_FILE_OK, gcu_file_stat(at("link").c_str(), &followed));
  ASSERT_EQ(GCU_FILE_OK, gcu_file_stat_link(at("link").c_str(), &itself));

  EXPECT_EQ(GCU_FILE_TYPE_REGULAR, followed.type);
  EXPECT_EQ(7u, followed.size);
  EXPECT_EQ(GCU_FILE_TYPE_SYMLINK, itself.type);
}

TEST_F(FileMeta, AMissingPathIsNotFoundRatherThanAnIoFailure) {
  GCU_File_Info info;
  EXPECT_EQ(GCU_FILE_ERR_NOT_FOUND,
      gcu_file_stat(at("nope").c_str(), &info));
  EXPECT_FALSE(gcu_file_exists(at("nope").c_str()));
  EXPECT_FALSE(gcu_file_exists(nullptr));
  put(at("yes"), "x");
  EXPECT_TRUE(gcu_file_exists(at("yes").c_str()));
}

TEST_F(FileMeta, ReadingAMissingFileSaysSoInsteadOfSayingIo) {
  // The distinction cjelly had to recover by asking the filesystem a second
  // question after the read had already failed.
  void * data = nullptr;
  size_t len = 0;
  EXPECT_EQ(GCU_FILE_ERR_NOT_FOUND,
      gcu_file_read(at("absent").c_str(), GCU_FILE_UNLIMITED, nullptr, &data,
          &len));
  EXPECT_EQ(nullptr, data);
}

TEST_F(FileMeta, ReadingADirectoryIsNotMistakenForAMissingFile) {
  // Both are failures; they are different failures, and the point of the new
  // code is that they do not collapse together.
  void * data = nullptr;
  size_t len = 0;
  GCU_File_Result r = gcu_file_read(dir.c_str(), GCU_FILE_UNLIMITED, nullptr,
      &data, &len);
  EXPECT_NE(GCU_FILE_OK, r);
  EXPECT_NE(GCU_FILE_ERR_NOT_FOUND, r);
  gcu_file_free(nullptr, data);
}

TEST_F(FileMeta, EveryDeterministicPathFailureIsNotFoundHoweverItIsSpelled) {
  void * data = nullptr;
  size_t len = 0;

  // Absent.
  EXPECT_EQ(GCU_FILE_ERR_NOT_FOUND,
      gcu_file_read(at("absent").c_str(), GCU_FILE_UNLIMITED, nullptr, &data,
          &len));

  // A component in the middle that is not a directory.
  put(at("plain"), "x");
  EXPECT_EQ(GCU_FILE_ERR_NOT_FOUND,
      gcu_file_read(at("plain/under").c_str(), GCU_FILE_UNLIMITED, nullptr,
          &data, &len));

  // A loop of symbolic links. None of these three will ever succeed on a
  // retry, and all three are statements about the path rather than about the
  // device - which is the line between NOT_FOUND and ERR_IO.
  ASSERT_EQ(0, symlink(at("loop_b").c_str(), at("loop_a").c_str()));
  ASSERT_EQ(0, symlink(at("loop_a").c_str(), at("loop_b").c_str()));
  EXPECT_EQ(GCU_FILE_ERR_NOT_FOUND,
      gcu_file_read(at("loop_a").c_str(), GCU_FILE_UNLIMITED, nullptr, &data,
          &len));

  GCU_File_Info info;
  EXPECT_EQ(GCU_FILE_ERR_NOT_FOUND, gcu_file_stat(at("loop_a").c_str(),
      &info));
  // The link itself is there, even though what it points at is not.
  EXPECT_EQ(GCU_FILE_OK, gcu_file_stat_link(at("loop_a").c_str(), &info));
  EXPECT_EQ(GCU_FILE_TYPE_SYMLINK, info.type);
}

TEST_F(FileMeta, APathTooLongToNameAnythingIsTheCallersMistakeNotAnAbsence) {
  // Equally deterministic, but it says the argument cannot name anything on
  // this filesystem rather than that nothing is there - so the caller's answer
  // is to fix its input, not to create the file.
  string huge = dir + "/" + string(5000, 'n');
  void * data = nullptr;
  size_t len = 0;
  EXPECT_EQ(GCU_FILE_ERR_INVALID,
      gcu_file_read(huge.c_str(), GCU_FILE_UNLIMITED, nullptr, &data, &len));
  GCU_File_Info info;
  EXPECT_EQ(GCU_FILE_ERR_INVALID, gcu_file_stat(huge.c_str(), &info));
}

TEST_F(FileMeta, ARealIoFailureStaysIoRatherThanBecomingNotFound) {
  // The other side of the line: a directory opens, so this is not a path
  // resolution failure, and it must not be reported as one.
  void * data = nullptr;
  size_t len = 0;
  GCU_File_Result r = gcu_file_read(dir.c_str(), GCU_FILE_UNLIMITED, nullptr,
      &data, &len);
  EXPECT_EQ(GCU_FILE_ERR_IO, r);
  gcu_file_free(nullptr, data);
}

TEST_F(FileMeta, RemoveDeletesAFileAndRefusesADirectory) {
  put(at("gone"), "x");
  ASSERT_TRUE(gcu_file_exists(at("gone").c_str()));
  EXPECT_EQ(GCU_FILE_OK, gcu_file_remove(at("gone").c_str()));
  EXPECT_FALSE(gcu_file_exists(at("gone").c_str()));

  EXPECT_EQ(GCU_FILE_ERR_NOT_FOUND, gcu_file_remove(at("gone").c_str()));
  // A directory is refused rather than removed, so that passing the wrong
  // variable cannot silently do the other operation.
  EXPECT_EQ(GCU_FILE_ERR_INVALID, gcu_file_remove(dir.c_str()));
  EXPECT_TRUE(gcu_file_is_directory(dir.c_str()));
}

TEST_F(FileMeta, RemoveTakesTheLinkAndNotWhatItPointsAt) {
  put(at("target"), "keep me");
  ASSERT_EQ(0, symlink(at("target").c_str(), at("link").c_str()));
  EXPECT_EQ(GCU_FILE_OK, gcu_file_remove(at("link").c_str()));
  EXPECT_TRUE(gcu_file_exists(at("target").c_str()));
}

TEST_F(FileMeta, RenameMovesAFileAndReplacesTheDestination) {
  put(at("a"), "first");
  put(at("b"), "second");
  EXPECT_EQ(GCU_FILE_OK,
      gcu_file_rename(at("a").c_str(), at("b").c_str()));
  EXPECT_FALSE(gcu_file_exists(at("a").c_str()));
  Read r(at("b"));
  ASSERT_EQ(GCU_FILE_OK, r.result);
  EXPECT_EQ("first", r.str());
  EXPECT_EQ(GCU_FILE_ERR_NOT_FOUND,
      gcu_file_rename(at("missing").c_str(), at("c").c_str()));
}

TEST_F(FileMeta, CopyDuplicatesTheContentAndLeavesTheSourceAlone) {
  put(at("from"), "payload");
  EXPECT_EQ(GCU_FILE_OK,
      gcu_file_copy(at("from").c_str(), at("to").c_str(), GCU_FILE_SYNC_FULL,
          GCU_FILE_PERMS_PRIVATE, nullptr));
  Read from(at("from")), to(at("to"));
  ASSERT_EQ(GCU_FILE_OK, from.result);
  ASSERT_EQ(GCU_FILE_OK, to.result);
  EXPECT_EQ("payload", from.str());
  EXPECT_EQ("payload", to.str());
}

TEST_F(FileMeta, CopyHandlesContentLargerThanOneChunk) {
  // The copy is streamed, so the interesting case is the one that goes round
  // the loop more than once.
  string big;
  for (int i = 0; i < 5000; ++i) {
    big += "0123456789";
  }
  put(at("from"), big);
  ASSERT_EQ(GCU_FILE_OK,
      gcu_file_copy(at("from").c_str(), at("to").c_str(), GCU_FILE_SYNC_NONE,
          GCU_FILE_PERMS_PRIVATE, nullptr));
  Read to(at("to"));
  ASSERT_EQ(GCU_FILE_OK, to.result);
  EXPECT_EQ(big.size(), to.len);
  EXPECT_EQ(big, to.str());
}

TEST_F(FileMeta, CopyOfAMissingSourceFailsAndWritesNothing) {
  EXPECT_EQ(GCU_FILE_ERR_NOT_FOUND,
      gcu_file_copy(at("nope").c_str(), at("to").c_str(), GCU_FILE_SYNC_NONE,
          GCU_FILE_PERMS_PRIVATE, nullptr));
  EXPECT_FALSE(gcu_file_exists(at("to").c_str()));
  EXPECT_TRUE(entries().empty());
}

TEST_F(FileMeta, CopyTakesThePermissionsAskedForNotTheSourcesOwn) {
  put(at("from"), "x");
  ASSERT_EQ(0, chmod(at("from").c_str(), 0666));
  ASSERT_EQ(GCU_FILE_OK,
      gcu_file_copy(at("from").c_str(), at("to").c_str(), GCU_FILE_SYNC_NONE,
          GCU_FILE_PERMS_PRIVATE, nullptr));
  // Not 0666: carrying the source's permissions across would be this library
  // making the access-model decision it declines to make.
  EXPECT_EQ(0600, mode_of(at("to")));
}

TEST_F(FileMeta, NullArgumentsAreRefusedRatherThanFatal) {
  GCU_File_Info info;
  EXPECT_EQ(GCU_FILE_ERR_INVALID, gcu_file_stat(nullptr, &info));
  EXPECT_EQ(GCU_FILE_ERR_INVALID, gcu_file_stat(at("x").c_str(), nullptr));
  EXPECT_EQ(GCU_FILE_ERR_INVALID, gcu_file_stat_link(nullptr, &info));
  EXPECT_EQ(GCU_FILE_ERR_INVALID, gcu_file_remove(nullptr));
  EXPECT_EQ(GCU_FILE_ERR_INVALID, gcu_file_rename(nullptr, at("x").c_str()));
  EXPECT_EQ(GCU_FILE_ERR_INVALID, gcu_file_rename(at("x").c_str(), nullptr));
  EXPECT_EQ(GCU_FILE_ERR_INVALID,
      gcu_file_copy(nullptr, at("x").c_str(), GCU_FILE_SYNC_NONE,
          GCU_FILE_PERMS_PRIVATE, nullptr));
  EXPECT_FALSE(gcu_file_is_directory(nullptr));
}
#endif

#ifndef _WIN32
using FileHandle = Scratch;

TEST_F(FileHandle, WriteThenReadBackThroughAHandle) {
  GCU_File_Handle h;
  ASSERT_EQ(GCU_FILE_OK,
      gcu_file_open(&h, at("f").c_str(), GCU_FILE_OPEN_WRITE,
          GCU_FILE_PERMS_DEFAULT, nullptr));
  EXPECT_EQ(GCU_FILE_OK, gcu_file_write_bytes(&h, "hello world", 11));
  EXPECT_EQ(GCU_FILE_OK, gcu_file_close(&h));

  ASSERT_EQ(GCU_FILE_OK,
      gcu_file_open(&h, at("f").c_str(), GCU_FILE_OPEN_READ,
          GCU_FILE_PERMS_DEFAULT, nullptr));
  char buf[32] = {0};
  size_t got = 0;
  ASSERT_EQ(GCU_FILE_OK, gcu_file_read_bytes(&h, buf, sizeof buf, &got));
  EXPECT_EQ(11u, got);
  EXPECT_EQ(string("hello world"), string(buf, got));
  EXPECT_EQ(GCU_FILE_OK, gcu_file_close(&h));
}

TEST_F(FileHandle, SeekAndTellReachTheMiddleOfAFile) {
  put(at("f"), "0123456789");
  GCU_File_Handle h;
  ASSERT_EQ(GCU_FILE_OK,
      gcu_file_open(&h, at("f").c_str(), GCU_FILE_OPEN_READ,
          GCU_FILE_PERMS_DEFAULT, nullptr));

  int64_t where = -1;
  ASSERT_EQ(GCU_FILE_OK, gcu_file_seek(&h, 4, GCU_FILE_SEEK_SET));
  ASSERT_EQ(GCU_FILE_OK, gcu_file_tell(&h, &where));
  EXPECT_EQ(4, where);

  char buf[3] = {0};
  size_t got = 0;
  ASSERT_EQ(GCU_FILE_OK, gcu_file_read_bytes(&h, buf, 2, &got));
  EXPECT_EQ(2u, got);
  EXPECT_EQ(string("45"), string(buf, got));

  ASSERT_EQ(GCU_FILE_OK, gcu_file_seek(&h, -3, GCU_FILE_SEEK_END));
  ASSERT_EQ(GCU_FILE_OK, gcu_file_tell(&h, &where));
  EXPECT_EQ(7, where);

  ASSERT_EQ(GCU_FILE_OK, gcu_file_seek(&h, 1, GCU_FILE_SEEK_CUR));
  ASSERT_EQ(GCU_FILE_OK, gcu_file_tell(&h, &where));
  EXPECT_EQ(8, where);
  EXPECT_EQ(GCU_FILE_OK, gcu_file_close(&h));
}

TEST_F(FileHandle, AShortReadIsTheEndOfTheFileAndNotAFailure) {
  put(at("f"), "abc");
  GCU_File_Handle h;
  ASSERT_EQ(GCU_FILE_OK,
      gcu_file_open(&h, at("f").c_str(), GCU_FILE_OPEN_READ,
          GCU_FILE_PERMS_DEFAULT, nullptr));
  char buf[16];
  size_t got = 0;
  EXPECT_EQ(GCU_FILE_OK, gcu_file_read_bytes(&h, buf, sizeof buf, &got));
  EXPECT_EQ(3u, got);
  // eof answers the read that already happened, which is the only question it
  // can answer.
  EXPECT_TRUE(gcu_file_eof(&h));

  EXPECT_EQ(GCU_FILE_OK, gcu_file_read_bytes(&h, buf, sizeof buf, &got));
  EXPECT_EQ(0u, got) << "reading past the end is zero bytes, not an error";
  EXPECT_EQ(GCU_FILE_OK, gcu_file_close(&h));
}

TEST_F(FileHandle, AppendGoesToTheEndAndWriteEmptiesFirst) {
  put(at("f"), "first");
  GCU_File_Handle h;
  ASSERT_EQ(GCU_FILE_OK,
      gcu_file_open(&h, at("f").c_str(), GCU_FILE_OPEN_APPEND,
          GCU_FILE_PERMS_DEFAULT, nullptr));
  ASSERT_EQ(GCU_FILE_OK, gcu_file_write_bytes(&h, "-second", 7));
  ASSERT_EQ(GCU_FILE_OK, gcu_file_close(&h));
  {
    Read r(at("f"));
    ASSERT_EQ(GCU_FILE_OK, r.result);
    EXPECT_EQ("first-second", r.str());
  }

  ASSERT_EQ(GCU_FILE_OK,
      gcu_file_open(&h, at("f").c_str(), GCU_FILE_OPEN_WRITE,
          GCU_FILE_PERMS_DEFAULT, nullptr));
  ASSERT_EQ(GCU_FILE_OK, gcu_file_write_bytes(&h, "new", 3));
  ASSERT_EQ(GCU_FILE_OK, gcu_file_close(&h));
  Read r(at("f"));
  ASSERT_EQ(GCU_FILE_OK, r.result);
  EXPECT_EQ("new", r.str());
}

TEST_F(FileHandle, UpdateWritesIntoTheMiddleWithoutTruncating) {
  put(at("f"), "0123456789");
  GCU_File_Handle h;
  ASSERT_EQ(GCU_FILE_OK,
      gcu_file_open(&h, at("f").c_str(), GCU_FILE_OPEN_UPDATE,
          GCU_FILE_PERMS_DEFAULT, nullptr));
  ASSERT_EQ(GCU_FILE_OK, gcu_file_seek(&h, 3, GCU_FILE_SEEK_SET));
  ASSERT_EQ(GCU_FILE_OK, gcu_file_write_bytes(&h, "XY", 2));
  ASSERT_EQ(GCU_FILE_OK, gcu_file_close(&h));

  Read r(at("f"));
  ASSERT_EQ(GCU_FILE_OK, r.result);
  EXPECT_EQ("012XY56789", r.str());
}

TEST_F(FileHandle, ReadAndUpdateRefuseAFileThatIsNotThere) {
  GCU_File_Handle h;
  EXPECT_EQ(GCU_FILE_ERR_NOT_FOUND,
      gcu_file_open(&h, at("absent").c_str(), GCU_FILE_OPEN_READ,
          GCU_FILE_PERMS_DEFAULT, nullptr));
  EXPECT_EQ(GCU_FILE_ERR_NOT_FOUND,
      gcu_file_open(&h, at("absent").c_str(), GCU_FILE_OPEN_UPDATE,
          GCU_FILE_PERMS_DEFAULT, nullptr));
  // Zeroed by a failed open, so closing it is safe and says nothing failed.
  EXPECT_EQ(GCU_FILE_OK, gcu_file_close(&h));
}

TEST_F(FileHandle, PermissionsApplyToAFileThisCallCreates) {
  mode_t saved = umask(0022);
  GCU_File_Handle h;
  ASSERT_EQ(GCU_FILE_OK,
      gcu_file_open(&h, at("private").c_str(), GCU_FILE_OPEN_WRITE,
          GCU_FILE_PERMS_PRIVATE, nullptr));
  ASSERT_EQ(GCU_FILE_OK, gcu_file_close(&h));
  EXPECT_EQ(0600, mode_of(at("private")));

  ASSERT_EQ(GCU_FILE_OK,
      gcu_file_open(&h, at("ordinary").c_str(), GCU_FILE_OPEN_WRITE,
          GCU_FILE_PERMS_DEFAULT, nullptr));
  ASSERT_EQ(GCU_FILE_OK, gcu_file_close(&h));
  umask(saved);
  // No probe needed on this path: open() applies the umask itself.
  EXPECT_EQ(0644, mode_of(at("ordinary")));
}

TEST_F(FileHandle, OpeningAnExistingFileDoesNotChangeItsPermissions) {
  put(at("f"), "x");
  ASSERT_EQ(0, chmod(at("f").c_str(), 0640));
  GCU_File_Handle h;
  ASSERT_EQ(GCU_FILE_OK,
      gcu_file_open(&h, at("f").c_str(), GCU_FILE_OPEN_WRITE,
          GCU_FILE_PERMS_PRIVATE, nullptr));
  ASSERT_EQ(GCU_FILE_OK, gcu_file_close(&h));
  EXPECT_EQ(0640, mode_of(at("f")));
}

TEST_F(FileHandle, SeekingPastTheEndMakesAGapThatReadsBackAsZeroes) {
  GCU_File_Handle h;
  ASSERT_EQ(GCU_FILE_OK,
      gcu_file_open(&h, at("sparse").c_str(), GCU_FILE_OPEN_WRITE,
          GCU_FILE_PERMS_DEFAULT, nullptr));
  ASSERT_EQ(GCU_FILE_OK, gcu_file_seek(&h, 8, GCU_FILE_SEEK_SET));
  ASSERT_EQ(GCU_FILE_OK, gcu_file_write_bytes(&h, "end", 3));
  ASSERT_EQ(GCU_FILE_OK, gcu_file_close(&h));

  Read r(at("sparse"));
  ASSERT_EQ(GCU_FILE_OK, r.result);
  ASSERT_EQ(11u, r.len);
  EXPECT_EQ(0, memcmp(r.chars(), "\0\0\0\0\0\0\0\0end", 11));
}

TEST_F(FileHandle, SeekingBeforeTheBeginningIsRefused) {
  put(at("f"), "abc");
  GCU_File_Handle h;
  ASSERT_EQ(GCU_FILE_OK,
      gcu_file_open(&h, at("f").c_str(), GCU_FILE_OPEN_READ,
          GCU_FILE_PERMS_DEFAULT, nullptr));
  EXPECT_NE(GCU_FILE_OK, gcu_file_seek(&h, -1, GCU_FILE_SEEK_SET));
  EXPECT_EQ(GCU_FILE_OK, gcu_file_close(&h));
}

TEST_F(FileHandle, FlushAndSyncBothReportSuccessOnAWritableFile) {
  GCU_File_Handle h;
  ASSERT_EQ(GCU_FILE_OK,
      gcu_file_open(&h, at("f").c_str(), GCU_FILE_OPEN_WRITE,
          GCU_FILE_PERMS_DEFAULT, nullptr));
  ASSERT_EQ(GCU_FILE_OK, gcu_file_write_bytes(&h, "durable", 7));
  EXPECT_EQ(GCU_FILE_OK, gcu_file_flush(&h));
  EXPECT_EQ(GCU_FILE_OK, gcu_file_sync(&h));
  EXPECT_EQ(GCU_FILE_OK, gcu_file_close(&h));
  Read r(at("f"));
  ASSERT_EQ(GCU_FILE_OK, r.result);
  EXPECT_EQ("durable", r.str());
}

TEST_F(FileHandle, CloseIsSafeTwiceAndOnAZeroedHandle) {
  GCU_File_Handle zeroed;
  memset(&zeroed, 0, sizeof zeroed);
  EXPECT_EQ(GCU_FILE_OK, gcu_file_close(&zeroed));
  EXPECT_EQ(GCU_FILE_OK, gcu_file_close(nullptr));

  GCU_File_Handle h;
  ASSERT_EQ(GCU_FILE_OK,
      gcu_file_open(&h, at("f").c_str(), GCU_FILE_OPEN_WRITE,
          GCU_FILE_PERMS_DEFAULT, nullptr));
  EXPECT_EQ(GCU_FILE_OK, gcu_file_close(&h));
  EXPECT_EQ(GCU_FILE_OK, gcu_file_close(&h));
}

TEST_F(FileHandle, NullArgumentsAreRefusedRatherThanFatal) {
  char buf[4];
  size_t got = 0;
  int64_t where = 0;
  GCU_File_Handle zeroed;
  memset(&zeroed, 0, sizeof zeroed);

  EXPECT_EQ(GCU_FILE_ERR_INVALID,
      gcu_file_open(nullptr, at("f").c_str(), GCU_FILE_OPEN_READ,
          GCU_FILE_PERMS_DEFAULT, nullptr));
  GCU_File_Handle h;
  EXPECT_EQ(GCU_FILE_ERR_INVALID,
      gcu_file_open(&h, nullptr, GCU_FILE_OPEN_READ, GCU_FILE_PERMS_DEFAULT,
          nullptr));
  EXPECT_EQ(GCU_FILE_ERR_INVALID,
      gcu_file_open(&h, at("f").c_str(), (GCU_File_Open_Mode)99,
          GCU_FILE_PERMS_DEFAULT, nullptr));
  EXPECT_EQ(GCU_FILE_ERR_INVALID,
      gcu_file_read_bytes(&zeroed, buf, sizeof buf, &got));
  EXPECT_EQ(GCU_FILE_ERR_INVALID, gcu_file_write_bytes(&zeroed, "x", 1));
  EXPECT_EQ(GCU_FILE_ERR_INVALID,
      gcu_file_seek(&zeroed, 0, GCU_FILE_SEEK_SET));
  EXPECT_EQ(GCU_FILE_ERR_INVALID, gcu_file_tell(&zeroed, &where));
  EXPECT_EQ(GCU_FILE_ERR_INVALID, gcu_file_flush(&zeroed));
  EXPECT_EQ(GCU_FILE_ERR_INVALID, gcu_file_sync(&zeroed));
  EXPECT_FALSE(gcu_file_eof(nullptr));
  EXPECT_FALSE(gcu_file_eof(&zeroed));
}

TEST_F(Scratch, SymlinkStoresTheTextItWasGivenAndReadLinkReturnsIt) {
  ASSERT_EQ(GCU_FILE_OK,
      gcu_file_symlink("target-text", at("link").c_str()));
  char * text = nullptr;
  size_t len = 0;
  ASSERT_EQ(GCU_FILE_OK,
      gcu_file_read_link(at("link").c_str(), nullptr, &text, &len));
  EXPECT_EQ(string("target-text"), string(text, len));
  gcu_file_free(nullptr, text);

  GCU_File_Info info;
  ASSERT_EQ(GCU_FILE_OK, gcu_file_stat_link(at("link").c_str(), &info));
  EXPECT_EQ(GCU_FILE_TYPE_SYMLINK, info.type);

  put(at("file"), "bytes");
  ASSERT_EQ(GCU_FILE_OK, gcu_file_symlink("file", at("alias").c_str()));
  GCU_File_Identity link_id;
  GCU_File_Identity file_id;
  ASSERT_EQ(GCU_FILE_OK, gcu_file_identity(at("alias").c_str(), &link_id));
  ASSERT_EQ(GCU_FILE_OK, gcu_file_identity(at("file").c_str(), &file_id));
  EXPECT_NE(link_id.inode, file_id.inode);

  std::string long_target(200, 'a');
  ASSERT_EQ(GCU_FILE_OK,
      gcu_file_symlink(long_target.c_str(), at("long").c_str()));
  ASSERT_EQ(GCU_FILE_OK,
      gcu_file_read_link(at("long").c_str(), nullptr, &text, &len));
  EXPECT_EQ(long_target, string(text, len));
  gcu_file_free(nullptr, text);
}

TEST_F(Scratch, HardLinkIsASecondNameForTheSameFile) {
  put(at("orig"), "same-bytes");
  ASSERT_EQ(GCU_FILE_OK,
      gcu_file_hardlink(at("orig").c_str(), at("alias").c_str()));
  GCU_File_Identity left;
  GCU_File_Identity right;
  ASSERT_EQ(GCU_FILE_OK, gcu_file_identity(at("orig").c_str(), &left));
  ASSERT_EQ(GCU_FILE_OK, gcu_file_identity(at("alias").c_str(), &right));
  EXPECT_EQ(left.device, right.device);
  EXPECT_EQ(left.inode, right.inode);
  EXPECT_GE(left.links, 2u);

  void * data = nullptr;
  size_t got = 0;
  ASSERT_EQ(GCU_FILE_OK,
      gcu_file_read(at("alias").c_str(), GCU_FILE_UNLIMITED, nullptr, &data,
          &got));
  EXPECT_EQ(string("same-bytes"), string((char *)data, got));
  gcu_file_free(nullptr, data);
}

TEST_F(Scratch, SetModeAppliesTheBitsItIsGiven) {
  put(at("f"), "x");
  ASSERT_EQ(GCU_FILE_OK, gcu_file_set_mode(at("f").c_str(), 0640));
  EXPECT_EQ(0640, mode_of(at("f")));
  GCU_File_Identity id;
  ASSERT_EQ(GCU_FILE_OK, gcu_file_identity(at("f").c_str(), &id));
  EXPECT_EQ(0640u, id.mode);

  ASSERT_EQ(GCU_FILE_OK, gcu_file_set_mode(at("f").c_str(), 04555));
  EXPECT_EQ(04555, mode_of(at("f")));

  put(at("target"), "y");
  ASSERT_EQ(GCU_FILE_OK, gcu_file_set_mode(at("target").c_str(), 0600));
  ASSERT_EQ(GCU_FILE_OK, gcu_file_symlink("target", at("via").c_str()));
  ASSERT_EQ(GCU_FILE_OK, gcu_file_set_mode(at("via").c_str(), 0644));
  GCU_File_Identity target_id;
  ASSERT_EQ(GCU_FILE_OK, gcu_file_identity(at("target").c_str(), &target_id));
  EXPECT_EQ(0644u, target_id.mode);
}

TEST_F(Scratch, LinkAndModeRefuseAMissingPathAndANullArgument) {
  EXPECT_EQ(GCU_FILE_ERR_INVALID, gcu_file_symlink(nullptr, at("l").c_str()));
  EXPECT_EQ(GCU_FILE_ERR_INVALID, gcu_file_symlink("t", nullptr));
  EXPECT_EQ(GCU_FILE_ERR_INVALID, gcu_file_hardlink(nullptr, at("l").c_str()));
  EXPECT_EQ(GCU_FILE_ERR_INVALID, gcu_file_hardlink(at("f").c_str(), nullptr));
  EXPECT_EQ(GCU_FILE_ERR_INVALID, gcu_file_set_mode(nullptr, 0644));
  EXPECT_EQ(GCU_FILE_ERR_INVALID, gcu_file_identity(at("f").c_str(), nullptr));
  char * text = nullptr;
  size_t len = 0;
  EXPECT_EQ(GCU_FILE_ERR_INVALID,
      gcu_file_read_link(nullptr, nullptr, &text, &len));
  EXPECT_EQ(GCU_FILE_ERR_NOT_FOUND,
      gcu_file_hardlink(at("missing").c_str(), at("alias").c_str()));
  EXPECT_EQ(GCU_FILE_ERR_NOT_FOUND,
      gcu_file_set_mode(at("missing").c_str(), 0644));
  EXPECT_EQ(GCU_FILE_ERR_NOT_FOUND,
      gcu_file_read_link(at("missing").c_str(), nullptr, &text, &len));
  EXPECT_EQ(nullptr, text);

  put(at("f"), "x");
  ASSERT_EQ(GCU_FILE_OK, gcu_file_symlink("t", at("link").c_str()));
  EXPECT_EQ(GCU_FILE_ERR_EXISTS,
      gcu_file_symlink("other", at("link").c_str()));
  EXPECT_EQ(GCU_FILE_ERR_EXISTS,
      gcu_file_hardlink(at("f").c_str(), at("link").c_str()));
  EXPECT_NE(GCU_FILE_OK,
      gcu_file_read_link(at("f").c_str(), nullptr, &text, &len));
  EXPECT_EQ(nullptr, text);
}
#endif

#ifdef _WIN32
TEST_F(Scratch, SetModeAndSymlinkDoNotApplyAndDoNotPretendTo) {
  put(at("f"), "x");
  EXPECT_EQ(GCU_FILE_ERR_ACCESS, gcu_file_set_mode(at("f").c_str(), 0640));
  EXPECT_EQ(GCU_FILE_ERR_NOT_FOUND,
      gcu_file_set_mode(at("missing").c_str(), 0644));
  EXPECT_EQ(GCU_FILE_ERR_ACCESS,
      gcu_file_symlink("target", at("link").c_str()));
  EXPECT_FALSE(gcu_file_exists(at("link").c_str()));
}
#endif

TEST(FileResultString, NamesEveryValueAndRefusesNone) {
  for (int i = 0; i < GCU_FILE_RESULT_COUNT; ++i) {
    const char * text = gcu_file_result_string((GCU_File_Result)i);
    ASSERT_NE(nullptr, text) << "result " << i;
    EXPECT_STRNE("unknown", text) << "result " << i << " has no name";
  }
  EXPECT_STREQ("unknown", gcu_file_result_string((GCU_File_Result)999));
}

int main(int argc, char** argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
