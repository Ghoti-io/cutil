# Files

## 1. What it is

What a program needs from a filesystem, with the parts that cannot be
described honestly on both POSIX and Windows left out rather than guessed at.

Whole files:  `gcu_file_read()` reads one into memory, and
`gcu_file_write_atomic()` replaces one without any reader ever seeing it partly
written.  `gcu_file_temp_create()` and `gcu_file_temp_commit()` are that second
one taken apart, for content that is produced rather than already in hand.

Everything else a caller reaches for on the first day:  `gcu_file_stat()` and
`gcu_file_exists()` to ask what is there, `gcu_file_remove()`,
`gcu_file_rename()` and `gcu_file_copy()` to move it about, `gcu_file_open()`
and the calls around it for a file too large to hold or one whose interesting
part is in the middle, and `dir.h` for the directories all of that lives in.

It is built on `path.h` and adds nothing of its own about path syntax.

Sections 2 to 8 are the whole-file half, which came first and carries most of
the reasoning.  Sections 9 to 13 are the rest, added when it became clear that
counting what this suite's own libraries called was the wrong way to decide
what a core library owes an arbitrary consumer.

## 2. Prior art

**The code quoted in this section has since been replaced.**  It is kept
because it is the specification this module was written against, and because
a reader deserves to see what was actually wrong rather than take the claim on
trust.  What happened when the libraries converted is at the end, and it
corrects one thing this section originally overstated.

The suite had written this several times before.

**Reading**:  `chron/src/zone/zonedb.c`, `cjelly/src/format/image.c`,
`ctang/src/tang.c`, `model/src/obj/obj_load.c`, `text/src/text_file_io.c` and
`text/src/yaml/yaml_file_io.c` each contained a whole-file reader.  Two of
them had independently converged on the same signature -

```c
GCHRON_Result gchron_zone_read_file(const char * path, size_t max_bytes,
    const GCHRON_Allocator * allocator, void ** out_data, size_t * out_len);
```

- a byte cap and an allocator, which is a strong signal about what the API
wants.  `cjelly`'s has neither, which is the untrusted-input hole the cap
exists to close.

**Atomic replacement**:  `text/src/text_file_io.c` and
`text/src/yaml/yaml_file_io.c` held the same mkstemp/fdopen/rename sequence
twice, and the copies had drifted apart.  On the `fdopen` failure path:

```c
/* text_file_io.c */              /* yaml_file_io.c */
_close(fd);                       _close(fd);
remove(temp_path);                free(temp_path);
free(temp_path);
```

The second forgot to remove the file it had just created, in **both** its
branches.  Neither copy called `fsync`, and both carried a comment claiming
the content was safe once `fflush` had returned, which is not what `fflush`
does.

### What the conversion found

All six readers above are now seams onto this module:  `text` converted in
`d4763d4`, `chron` in `e0ed78f`, `model` in `83d7c65`, `ctang` in `a2c3bf5`
and `cjelly` in `ec913e5`.

The conversion turned up a defect neither this section nor the duplication
itself had predicted, and it is the one that mattered - in three of the six
copies, which had never shared a line of code:

- **YAML could not read a stream.**  Its copy read with `fseek`/`ftell`/
  `fread`, so a pipe, a FIFO, `/dev/stdin` or anything under `/proc` was
  refused outright with "Failed to seek file" - while `text`'s README had
  claimed otherwise for all three formats for months.  That is the only
  difference between the copies a caller could actually see, and it is
  precisely the failure section 3 describes.  `text` has a test for it that
  fails against the previous implementation.
- **Neither could `cjelly`, and it said the wrong thing about it.**  Its copy
  sized with `fseek`/`ftell` in the same way, so the same inputs came back
  empty - and an image reader cannot tell an empty buffer from a bad one, so
  the failure was reported as a corrupt image rather than as a file that could
  not be read that way.  It too has a test that fails against the old reader.
- **`ctang` sized with `ftell` on a stream opened in text mode**, which
  over-reports on Windows by the number of line endings.  The tail of the
  buffer was then whatever the allocator last left there, and it was handed to
  the compiler as source.  That copy also ignored what `fread` returned and
  leaked the `FILE *` when the allocation failed.

Three copies, written separately, with the same shape at the bottom of each.
That is a better argument for one implementation than the duplication itself
was:  duplication predicts that the copies will *differ*, and this section
originally went looking on exactly those grounds - but what was actually wrong
is the thing all three agreed on.

Two further differences turned out to be smaller than this section originally
called them, and the record should say so.  It said "three defects"; two of
the three are better described as divergences between copies than as bugs
anyone met, and `text`'s tests for both hold against the old code as well:

- YAML never applied its own `max_total_bytes` to the *file*, so an over-large
  document was read into memory in full and refused afterwards by the parser.
  The answer was the same either way; the limit had simply already been spent
  by the time it was applied.  A capped read closes that by construction.
- The missing `remove()` on the `fdopen` failure path sits on a path that is
  close to unreachable.  It was a real difference between two copies of one
  function, which is the argument for there being one function - but not an
  incident.

One deliberate behaviour change came with the conversion rather than out of
it:  `text` now commits the bytes before the rename instead of leaving them to
writeback.  The hand-written version flushed stdio and renamed, which survives
a killed process but not a power loss, and these parsers are usually pointed
at configuration files.  That is `GCU_FILE_SYNC_FULL` being the zero value,
doing what section 6 says it is for.

## 3. Reading in chunks, not by size

`gcu_file_read()` never asks how large the file is.  It reads until the read
stops producing bytes.

Sizing first - seek to the end, tell, allocate, read - is the obvious
implementation and it returns nothing at all for any file whose size the
kernel does not know in advance.  Every file under `/proc` reports zero;
`/proc/version` on this machine reports a size of 0 and yields 213 bytes.
Pipes and character devices behave the same way.  `FileRead.WorksOnAFileThat
ReportsNoSize` exists because that failure is silent: the call succeeds and
hands back an empty buffer.

The buffer always carries a NUL one byte past `out_len`, which is not counted
in it.  A text caller can use the result as a C string with no copy; a binary
caller ignores it.  `ReadKeepsEmbeddedNulBytes` checks that the added
terminator is not confused with content, since the length is what delimits the
data and the NUL is only a convenience.

`max_bytes` is a promise and not a truncation, in the same sense as section 5
of `CONVENTIONS.md`.  A file over the limit yields `GCU_FILE_ERR_LIMIT` and
nothing is allocated.  The read never holds more than one byte over the limit
while discovering this, which is the byte that proves the file is too big.

## 4. The handle, and the bug it makes impossible

The two hand-written copies handed back a bare `char *` and a `FILE *`,
leaving every error path to remember `remove()`.  One of nine such paths
forgot.  That is not carelessness so much as a shape that invites it: cleanup
is a thing to remember rather than a thing to call.  Section 2 records how
little that particular omission cost in practice - the path is close to
unreachable - which is the argument for the shape rather than against it.  The
same shape is what let the *reachable* difference between those copies go
unnoticed for months.

`GCU_File_Temp` is disposed of by exactly one of two named functions:

- `gcu_file_temp_commit()` - close, move into place, done.
- `gcu_file_temp_abort()` - close, delete, done.

Both leave the handle zeroed, and `abort` accepts a zeroed handle.  So `abort`
can sit on an unconditional cleanup path without anybody tracking whether
`commit` already ran, which is what removes the class of mistake rather than
just this instance of it.  `TempAbortIsSafeOnAHandleThatWasNeverOpenedOr
IsAlreadySpent` pins all five cases: NULL, zeroed, after a failed create,
twice, and after a commit.

A failed `commit` removes its own temporary too, leaves the destination
exactly as it was, and still spends the handle.

## 5. What atomic means here, and what it needs

`rename()` is atomic with respect to the directory entry:  a reader opening
the destination gets the whole old file or the whole new one, never a mixture,
and never a zero-length window in between.  That holds **only if the temporary
file is on the same filesystem**, because a rename across filesystems is a
copy followed by a delete, and a copy is not atomic.

This is why `gcu_file_temp_create()` takes a directory and why
`gcu_file_write_atomic()` passes the destination's own.  Reaching for the
system temporary directory instead would work on a developer's laptop, where
everything is one filesystem, and fail in production where `/tmp` is separate.
`WriteAtomicDoesNotUseTheSystemTemporaryDirectory` makes that observable by
pointing `$TMPDIR` at nothing: the correct implementation never looks there.

## 6. Durability, stated exactly

`fflush()` moves bytes out of stdio and into the kernel.  It does not put them
on a disk.  A rename recorded while the content behind it is not leaves a file
that exists, has the right name, and is empty.

`GCU_FILE_SYNC_FULL` is the zero value, so a caller who does not think about
it gets the durable behaviour:

1. `fflush` the stream.
2. `fsync` it - **a failure here is reported**, because these are the bytes.
3. `fclose`, before the rename, because Windows will not move an open file.
4. Rename.
5. `fsync` the destination's directory - **a failure here is not reported**.

Step 5 is deliberately weaker.  Several filesystems refuse `fsync` on a
directory outright, and failing a replacement that has otherwise completed
would be worse than the promise being softer.  The promise is therefore:  with
`SYNC_FULL`, the content is on the disk before the rename is attempted, and
the directory entry is committed on a best-effort basis.  A crash in the
window between steps 4 and 5 can leave the old file in place, but cannot leave
a truncated or empty new one.

`GCU_FILE_SYNC_NONE` skips steps 2 and 5.  The rename is still atomic; only
the durability is given up.  That is the right choice for output that can be
regenerated and the wrong one for anything else.

Neither copy in `text` did any of this: both flushed stdio and renamed, which
survives a killed process but not a power loss.  It is the part of section 2
that most deserved to be one decision rather than an omission repeated per
call site, and converting `text` changed the behaviour of all three of its
formats accordingly.

## 7. Taking the name and the file in one step

`mkstemp()` chooses a name, creates the file, and fails if the name is already
taken - all in one operation, with the file opened `0600`.

Choosing a name and then opening it is the classic local privilege-escalation
shape: between the two steps, anything that can write to the directory can put
a symbolic link at that name, and the open follows it.  In a world-writable
`/tmp` that is a real attack and not a theoretical one, which is why the
Windows branch pairs `_wmktemp_s` with `_O_CREAT | _O_EXCL` rather than
trusting the name it was handed.

`TempCreateOpensAFileOnlyItsOwnerCanRead` asserts the mode, because the
permissions are the half that a refactor can quietly lose:  the file is
usually the place a secret is being written *through*.

That is the temporary file, for its whole life.  What the *destination* ends
up with is the next section, and is a separate question - it was not always,
and that is what the next section is about.

## 8. Permissions, and who decides them

For a while this module answered that question by not asking it.  The
destination was renamed from a `mkstemp` temporary and inherited its `0600`,
so five libraries got owner-only files without anybody choosing that:
`cjelly`'s captured screenshots came out `-rw-------` where the `fopen` they
replaced had made them `-rw-rw-r--`.  The behaviour was not obviously wrong -
it is the conservative direction - but it was not written down anywhere, and a
contract nobody stated is one nobody can be said to have accepted.

The design says permissions are the caller's business.  An access model cannot
be described honestly across platforms:  POSIX derives a new file's
permissions from a process-global umask, Windows has no umask at all and
inherits access control entries from the parent directory.  A library that
claimed to model both would be lying on at least one.  So this module does not
model them - but a caller can only take on a responsibility it has been told
about, which is what the silence prevented.

`GCU_File_Perms` is therefore three values and not a `mode_t`.  It asks the
only question that can be answered on both platforms:

| Value | The finished file |
| --- | --- |
| `GCU_FILE_PERMS_PRIVATE` | Owner-only.  The zero value. |
| `GCU_FILE_PERMS_DEFAULT` | What an ordinary `fopen()` here would have made. |
| `GCU_FILE_PERMS_PRESERVE` | What the destination already had, else `DEFAULT`. |

`PRIVATE` is the zero value for the same reason `SYNC_FULL` is:  a caller who
does not think about it should not publish something by omission.  It also
means the change that introduced this enum altered nobody's behaviour - the
five libraries that had already converted kept exactly what they had, and got
a way to say otherwise.

`PRESERVE` is the one worth arguing for.  Replacing a file is not the same act
as creating one:  somebody who runs `chmod 600` on a configuration file has
said something, and rewriting that file is not an occasion to un-say it.  The
reverse holds too - a file deliberately made group-readable should not become
private because it was edited.  Neither of the other two values can express
"leave this as the operator left it".

### Asking rather than computing

`DEFAULT` does not calculate `0666 & ~umask`.  It creates an empty file beside
the temporary, asks the kernel what mode it got, removes it, and uses that.

Two reasons, and the second is the one that decided it.  The umask is
process-global and the only portable way to read it is to set it -
`umask(0)` and then put it back - which is a window in which every other
thread creating a file gets `0666`.  And the answer is not a function of the
umask anyway.  A *default ACL* on the containing directory overrides the umask
entirely; on this machine, a directory carrying one produced a `0664` file
under `umask 0077`.  A computed answer is wrong on exactly the systems whose
administrator went to the trouble of configuring one, which is the worst
possible place to be wrong.

The probe is opened `O_CREAT | O_EXCL`, for the same reason the temporary file
is:  a name in a directory other people can write to is not a file until it
has been created as one.  `TheProbeRefusesANameSomebodyElseAlreadyHolds`
plants a symbolic link at the name the probe will use and checks that the
target is untouched and the call fails.

### Before the rename, not after

The permissions are applied while the temporary is still a temporary, through
its own descriptor, immediately before the move.  The rename is the moment the
file becomes reachable under a name somebody else knows, so doing it
afterwards would leave a window in which the destination exists with the
*wrong* permissions - and on a file whose content is already complete.

A failure to apply them fails the call and leaves the destination alone.  A
file with permissions other than the ones asked for is a different file from
the one requested, and quietly handing it over would be the same class of
silence this section exists to end.

### What is deliberately missing

Ownership, ACLs, extended attributes, inherited groups, the read-only
attribute on Windows, and anything expressed as a `mode_t`.  Those are the
caller's, with its own platform's API.  What changed is that the caller is now
told what it is starting from.

On Windows none of this is implemented:  `_wchmod` moves only the read-only
attribute, and the entries that actually decide access are inherited from the
destination's directory when the file is created - so `DEFAULT` and `PRESERVE`
already hold there and `PRIVATE` does **not**.  Making it hold needs
`SetSecurityInfo` and a constructed DACL.  Like the rest of the Windows branch
in this module, it has never been compiled or run.

## 9. Telling one failure from another

`GCU_File_Result` distinguishes four things beyond the obvious:
`ERR_NOT_FOUND`, `ERR_EXISTS`, `ERR_ACCESS` and `ERR_NOT_EMPTY`.  Everything
else is `ERR_IO`, because nobody can usefully branch on the difference between
`EIO` and `ENXIO`.

The dividing line is **determinism**, not severity.  `ERR_IO` means the
filesystem tried and something went wrong - a condition a caller may reasonably
retry, or report as a device problem.  A failure to resolve the path is not
that:  it will never succeed on a retry, and it is a statement about the path
rather than about the device.  So all three spellings of it come back the same
way - the path is absent (`ENOENT`), a component of it is not a directory
(`ENOTDIR`), or it goes round a loop of symbolic links (`ELOOP`).  Grouping a
deterministic path failure with retryable I/O is the mapping that would
actually mislead somebody.

`ENAMETOOLONG` looks like it belongs with those and does not.  It is equally
deterministic, but it says the *caller's argument* cannot name anything on this
filesystem rather than that nothing is there, and the answer to it is to fix
the input rather than to create the file.  That is `ERR_INVALID`.

That line was drawn by two readers rather than one.  The first mapping had only
`ENOENT` and `ENOTDIR` on it, and `ELOOP` and `ENAMETOOLONG` fell through to
`ERR_IO` - which is the inconsistency you get from listing cases instead of
naming a principle.

The line was drawn there by watching what happened when it was not.  For a
while `gcu_file_read()` reported one `ERR_IO` for a path that was absent and
for a file that would not open, on the reasoning that both are failures.
`cjelly` needed them apart - a wrong path is the caller's mistake and a failing
disk is not - so it asked the filesystem a second time after the read had
already failed, calling `gcu_path_canonicalize()` and throwing away the
canonical path it allocated, with a paragraph of comment explaining why:

> `gcu_path_canonicalize()` is the cross-platform existence query

It is not.  It is a symlink resolver being used to answer a yes/no question,
because there was nothing else - and the second question is a race as well as a
duplication, since the file may appear or vanish between the two.  That is a
consumer working around a missing primitive, and it is the clearest signal in
this repository that the missing thing was missing.

### Map this enum with a `default:`

`GCU_File_Result` grew by four members in one commit, and it will grow again.
In C that is a **source-breaking change for every exhaustive `switch` over
it**:  `-Wall` implies `-Wswitch`, every library here builds `-Werror`, and a
`switch` that names each member and has no `default:` stops compiling the
moment a member is added.  Adding `GCU_FILE_ERR_NOT_FOUND` and its three
companions broke three such switches across the suite - in libraries that never
called either function whose signature changed, which is why a search for call
sites did not find them.

### A `default:` is necessary and not sufficient

Adding one silences `-Wswitch`, so a `switch` that *has* a default keeps
compiling and changes behaviour without saying anything.  That makes the
substance of the rule not "have a default" but **what the default maps to**,
and it means a survey for broken mappings cannot be done with a compiler.

`model/src/stream/stream_memory.c` is the worked example, and the honest
version of it is that this library broke it rather than that it knew better.
Its fallback was `GMDL_ERR_INTERNAL` on the reasoning that anything unrecognised
was a contract violation.  Four new members arrived, `gmdl_obj_load_file()`
began reporting "internal library error" for a file that had simply been
deleted, and three of its tests would have gone red - `ObjLoad`, `MtlLoad` and
`StreamFile` each have a `MissingFileReportsIo`.  It was fixed in `e939765`,
whose comment says it plainly:  "cutil's enumeration grows", which "turned each
addition into 'internal library error' for a caller whose file had simply been
deleted".

That sentence was written *after* this change, not before it.  An earlier draft
of this section cited it as prior art - as a library that had worked the rule
out independently - which read better and was false.  It is a reaction, and it
is worth quoting for its reasoning rather than for its foresight.

So map a `GCU_File_Result` the way that file now does.  Name the members you act on
and let a `default:` carry the rest onto your own general I/O failure.  That
keeps the mapping compiling across additions *and* degrades honestly:  a result
you have never heard of is some kind of I/O problem, which is true, rather than
an internal error, which is not.

The rule inverts inside this library, and `gcu_file_result_string()` is the
demonstration:  its `switch` names every member and has **no** `default:`, on
purpose.  It is the one place that must not silently cope with a new member -
a result with no name would print as `"unknown"` to somebody trying to
diagnose a failure - so the missing `default:` is what makes the compiler stop
the build until the name exists.  Deliberately brittle in the one function
whose job is to be complete, tolerant everywhere else.  A consumer wants the
opposite of what the owner wants, and the same construct expresses both.

**Translate defensively, name exhaustively.**  A caller mapping this enum onto
its own vocabulary wants a `default:`; the function whose job is to name every
member wants the compiler to fail without one.

### Finding the three shapes

A change to this enum reaches three kinds of caller, and only the first is
loud.  All three want a different search, which is why the call-site list was
the wrong instrument:  the affected set is *reads a result*, not *writes a
file*.

| Shape | Noticed by | How to find it |
| --- | --- | --- |
| `switch`, no `default:` | the compiler | any `switch` block naming `GCU_FILE_` without a `default:` |
| `switch` with a `default:` | nothing | the same blocks, reading what the default arm returns |
| `== GCU_FILE_ERR_...` | nothing | grep for equality against a specific member |

The middle row is the one that was missed.  Two sweeps were run on this change
and neither covered it, so `model` was classified safe on the strength of
having a `default:` at all - which is exactly the reasoning this section now
warns against.

### A fourth shape, which the caller cannot find at all

There is a fourth, and it is worse than the three above because nothing in the
caller's code changes and nothing in the caller's code can reveal it.  A member
the caller already *names* can have its **input set widened** while the name
stays put.

The old `gcu_file_read()` inspected no `errno`:  every open failure was a flat
`ERR_IO`.  Classifying them moved two errnos onto members that already existed:

| errno | was | is |
| --- | --- | --- |
| `ENAMETOOLONG` | `ERR_IO` | `GCU_FILE_ERR_INVALID` |
| `ENOMEM` | `ERR_IO` | `GCU_FILE_ERR_OOM` |

A caller with `case GCU_FILE_ERR_INVALID:` still compiles, still has a
`default:`, has no equality test, and now answers "the caller's argument is
wrong" for a path that is merely too long for this filesystem.  Searching that
caller for switches, defaults or comparisons finds nothing, because the caller
is unchanged and correct as written.

**Only the library that moved the errnos can find this.**  The question is not
"what does your code do with the enum" but "for each value I reclassified, who
names the destination" - and that is a question about the classification table,
which the caller has never seen.

That question yields *candidates*, not victims, and the difference matters:  the
first run of it here reported every remaining library as affected, when one of
them already gave the right answer.  `text` names `ERR_INVALID` but falls it
through to the same arm as `ERR_IO`, so an over-long path was always going to
come back as an I/O failure there - correct, and correct by accident of how the
cases were grouped rather than by decision.  Naming a member is the search;
what the arm *returns* is the finding.  Which is the "necessary and not
sufficient" point above, one level up, made twice in the same afternoon.

The worst instance found was `cjelly/src/format/image.c:152`, where
`ERR_INVALID` maps to `CJELLY_FORMAT_IMAGE_ERR_INVALID_FORMAT`:  a path too
long for the mount would be reported as a malformed image, and somebody would
go and debug a PNG that is fine.

### And a fifth, which is the fourth one's consequence

A mislabelled result is bad.  A result that some *other* part of the same
library branches on is worse, because the mapping change then reaches code that
never mentions this enum at all.

`chron` found the instance.  `gchron_zonedb_zone()` consults the tzdb's
backward-compatibility link table only when the read comes back
`GCHRON_ERR_IO`.  Once an over-long directory path started arriving as
`GCHRON_ERR_INVALID` instead, that lookup was skipped - so `US/Eastern` and
`Asia/Calcutta` stopped resolving, on a path whose only fault was its length.
A change to an error mapping silently disabled a feature two hundred lines
away, and it is reachable from the public API:

```
gchron_zonedb_directory(<5000 chars>, ...)   -> GCHRON_OK
gchron_zonedb_zone(db, "Europe/Paris", &z)   -> GCHRON_ERR_INVALID
```

So the question after "what does this arm now report" is **"what else in this
library branches on the value it used to report"**.  That one the caller *can*
ask, and only the caller can - it is a question about their own code, not about
this classification table.  The two halves of the search live on opposite sides
of the library boundary, which is why neither side found this alone.

It was checked in the others and is chron's alone:  `compress` maps only
`!= GCU_FILE_OK`, and `cjelly`'s own codes are compared in tests but never
branched on in library code.  That is a negative worth recording, because a
shape found once looks like the first of many until somebody looks.  A library that changes an error vocabulary
therefore owes its callers *that* list, not a list of renamed functions.  The
impact list for this change was wrong eight times, and each time it was wrong
because it described what had been changed rather than what the change reached.

`model` found this one, in `3f42591`, after having already been fixed once for
the shape above.  Its resolution is worth recording:  it maps `ERR_INVALID` to
`GMDL_ERR_IO` rather than to its own `GMDL_ERR_INVALID`, because `NAME_MAX` is
per-filesystem - the same path is too long on one mount and fine on another,
which is not a caller misusing an API.  It also records the precondition that
makes reading `ERR_INVALID` as "bad path" safe at all:  its own entry point
rejects NULL before calling, so this library is never reporting an argument
`model` passed.  A caller without that precondition must not do the same, or a
real argument bug and an over-long path become the same answer.

### Splitting a code is a decision, not a conversion

The same caution applies in the other direction, and it does not announce
itself at compile time.  Before this change the open failure was a flat
`ERR_IO` with no `errno` inspected at all, so `ERR_IO` was the only spelling
available for *every* reason an open can fail.  Code that tested for it was not
saying what it meant; it was saying the most it could.

That matters because one old code now maps to several new ones, and choosing
which of them to test is a **behaviour decision wearing the costume of a
rename**.  `chron`'s leap-second reader is the worked example.  It falls back
from `$TZDIR` to the system table on `ERR_IO`, so today it falls back when the
file is absent, when the path loops, *and* when the file exists but cannot be
opened for permissions.  Afterwards those are three different codes, and three
different conditions are defensible:

| Condition | Means |
| --- | --- |
| `NOT_FOUND` | nothing was there |
| `NOT_FOUND \|\| ACCESS` | we never got it open - today's behaviour |
| `+ INVALID` | the above, plus a path this filesystem cannot use |

The comment above that condition argues for two of the three, which is the
point:  the original author could not have distinguished them, so the code
cannot tell you which they wanted.  Only a person can settle it, and the commit
that settles it should say so - the next reader will otherwise assume it was
mechanical.

It was settled twice, and the second answer came from a question none of the
three options asked.  `chron` first took the middle row, then reversed to the
third in `22d6506` on a point that had been sitting in the same function all
along:  that reader *already* treats `$TZDIR` naming a missing directory, a
plain file, or a loop of symbolic links as an ordinary miss.  A path too long
is the fourth spelling of the same misconfiguration, so refusing only that one
is a rule with nothing behind it - and the refusal surfaced nothing either,
since `GCHRON_ERR_IO` does not tell anybody their `$TZDIR` was too long.  It
lost a working answer and reported no reason.

The transferable form, for anyone mapping this vocabulary onto a **search
path** rather than a single open:  the question is not "is this errno more like
absence or more like an error" but **"does this location already miss silently
for reasons that are just as bad"**.  Where it does, the new spelling joins its
siblings.  `chron`'s rule came out as failing to *obtain* the file sends the
search on - `NOT_FOUND`, `ACCESS`, `INVALID` - while a failure about the file's
contents or about the process is the answer:  `LIMIT`, a mid-read `ERR_IO`, and
`OOM`.

That last boundary is worth naming because it looks wrong and is not.  `OOM`
reads like it belongs with the fall-through group, and a mutation moving it
there survived `chron`'s first test pass:  with an allocator that refuses
everything, the fallback read fails too and both answers are the same, so the
test passes against the mutant.  Pinning it needs an allocator that refuses its
**first** request and grants the rest.  Any caller with a fall-through boundary
of its own needs a harness shaped like that, not a blanket failure.

Nobody would have found this by reading the diff of the library that changed.
It was found by the session that owned the caller, corrected by a third session
that read the *old* cutil source rather than reasoning about what it must have
done, and it never stops compiling at any point.  Where the old code could not
say what it meant, re-read the condition; do not translate it.

## 10. Asking what is there

`gcu_file_stat()` fills in a `GCU_File_Info`:  a type, a size, and a
modification time.  Nothing else.

Ownership, permissions, link counts, device numbers and the several kinds of
timestamp a platform may or may not keep are all absent for the reason section
8 gives.  A struct with a `uid` field would be lying on Windows; one with a
Windows security descriptor would be lying everywhere else.

The time is `int64_t` nanoseconds since the Unix epoch, and is deliberately
**not** a `chron` type:  `chron` is built on this library, so depending on it
here would invert the suite.  Windows counts 100-nanosecond ticks from 1601 and
is converted at the boundary.  The resolution is what the filesystem kept -
many record whole seconds - so this is the value converted, not the precision
promised.

`gcu_file_stat_link()` does not follow a symbolic link at the end of the path.
The distinction matters to anything that walks a tree, because following links
is how a walk leaves the tree it was asked about.

`gcu_file_exists()` is the convenience, and its documentation says plainly what
it cannot do:  it answers "no" both for a path that is absent and for one the
caller may not look at, and the answer is history by the time it returns.  It is
for a better error message, not for deciding that a later operation will work.

## 11. Directories

`gcu_dir_create()` makes one level.  `gcu_dir_create_all()` builds a path, and
succeeds when the directory is already there - the caller asked for it to
exist, not for it to be new.  It does *not* succeed when the name is taken by
something that is not a directory:  that is a collision, and reporting it as
success would hand back a path that cannot be written into.

The walk is an iterator - `gcu_dir_open()`, `gcu_dir_read()`,
`gcu_dir_close()` - rather than a function returning a list.  A directory can
hold more entries than a caller can afford to hold at once, and a caller is
usually looking for one of them.

`"."` and `".."` are never reported.  They are an artefact of how a filesystem
stores a directory rather than things in it, and every caller that has ever
forgotten to skip them has walked its own parent.

Entry types come back without following links, so a walk cannot be led out of
its tree.  Where the platform will not say - `DT_UNKNOWN`, which XFS without
`ftype` and several network filesystems answer for everything - the entry is
asked about directly rather than reported as `OTHER`, which would otherwise
make every entry typeless on exactly those systems.

There is **no recursive delete**, and that is a decision rather than an
oversight.  It is the operation that most wants a symbolic link followed out of
it by mistake, and the failure mode is deleting something nobody pointed at.  A
caller who wants one can write it over `gcu_dir_read()`, whose types are
already link-safe; what it needs first is a conversation about what guards it
should carry.

## 12. Opening a file and moving around in it

`gcu_file_open()` exists for the two cases whole-file reading cannot serve:  a
file too large to hold, and one whose interesting part is somewhere in the
middle.

It is thin on purpose.  `fopen` and `fread` are already standard C, and
wrapping them again would buy nothing.  What it adds is the three things stdio
does *not* get right across platforms:

- **UTF-8 paths.**  `fopen` on Windows cannot open a path whose bytes are
  UTF-8.  This opens through the wide entry point, as section 14 describes.
- **Offsets past 2 GB.**  `ftell` returns `long`, which is 32 bits on 64-bit
  Windows, so seeking a large file through plain stdio silently stops working
  at two gigabytes.  These offsets are 64-bit everywhere.
- **Line endings.**  Always binary.  A text-mode read on Windows rewrites the
  bytes and makes the offset disagree with how many there are - which is
  exactly the bug `ctang` had, recorded in section 2.

Two asymmetries are deliberate.  A short **read** is not a failure:  it is the
end of the file, and the count is reported rather than demanded, because
anything reading a stream has to cope with it.  A short **write** is a failure,
because there is no benign reason for one and a caller that carried on would be
building a file with a hole in it.

`gcu_file_close()` returns a result, and it is worth reading.  Buffered output
reaches the operating system at the close, so the close is the call that
reports a full disk.  Ignoring it is how a program loses the last few kilobytes
of what it wrote and never finds out.

Permissions on this path need no probe, unlike section 8's:  `open()` applies
the umask itself, so asking for `0666` gets whatever the platform would have
given any other new file.  The probe exists over there only because `mkstemp()`
forces `0600` and the mode has to be recovered afterwards.

## 13. Matching names

`gcu_path_match()` is in `path.h` rather than here, because it is a question
about a string and touches no filesystem at all.  It is documented with the
rest of the path work; the reason it exists is this module - filtering a
directory walk by name is one of the first things a caller wants, and
`gcu_dir_read()` plus a matcher is how that is spelled.

It needs no regular-expression engine, which is what keeps this library free of
that dependency.

## 14. Encoding

Paths are UTF-8, converted to UTF-16 at the Win32 boundary, exactly as in
`path.h` - `_wfopen`, not `fopen`.  The conversion helpers live in
`src/path_internal.h` as `static inline` so that `path.c` and `file.c` share
one implementation rather than each carrying a copy of Windows-only code
nobody here can compile.  A second copy of that is precisely the thing this
module exists to stop.

## 15. What it does not do

Two entries that used to be here have gone, and the reason they were here is
worth keeping.  "No directory creation" and "no partial reads or writes" were
both justified by the six whole-file readers in section 2 not needing them -
which measures those six callers, not what a core library owes a consumer who
has never heard of them.  A successful workaround, like `cjelly`'s in section
9, looks exactly like an absence of demand.  What follows is meant to be the
list of things genuinely decided against.

- **No parser streams.**  Section 5 of `CONVENTIONS.md` gives parsing libraries
  their own `<PREFIX>_Stream`, with pull semantics, pushback and an incremental
  contract.  `gcu_file_open()` is not that and does not try to be:  it is a
  file, not a parse.
- **No recursive delete.**  Section 11 says why:  it is the operation most
  likely to follow a symbolic link out of the tree it was given, and it wants a
  design conversation rather than a convenience function.
- **No locking.**  Two processes replacing the same file race, and the loser's
  content is simply gone.  Advisory locking is a separate decision with a
  separate set of platform lies attached to it.
- **No ownership, ACLs or extended attributes.**  Section 8.
- **No file watching.**  `inotify`, `kqueue` and `ReadDirectoryChangesW` agree
  on almost nothing, including whether a rename is one event or two.
- **Symbolic link creation fails closed on Windows.**  Reading a link's type
  is supported because every walk needs it. Making one needs a privilege there
  that a normal user does not have. `gcu_file_symlink()` exists because a
  caller extracting an archive has to create the link or refuse it, and an
  absent function would push that caller into `open` and `symlink` itself.
  On Windows the call returns `GCU_FILE_ERR_ACCESS` and creates nothing — it
  does not report success, and it does not write an ordinary file where the
  link would have been. `gcu_file_set_mode()` is the same shape for a Unix
  mode, which does not apply there: the error is real, and a missing path is
  still `GCU_FILE_ERR_NOT_FOUND` so the two are not one answer.

## 16. Testing

`test/test-file.cpp`, 72 tests, and `test/test-dir.cpp`, 17.  Both clean under
ASan+UBSan and under Valgrind with `--leak-check=full`.  The matcher's tests
live with the rest of the path work.

Checked by sabotage.  Each invariant was broken in the source and the suite
confirmed to fail:

| Sabotage | Tests failing |
| --- | --- |
| a read over the limit truncates instead of refusing | 2 |
| the buffer is not NUL-terminated past its length | 1 |
| outputs are written even when the read failed | 2 |
| `abort` leaves the temporary file on disk | 1 |
| `abort` does not empty the handle | 1 (a double free) |
| `commit` leaves its temporary behind when the rename fails | 1 |
| `write_atomic` uses the system temporary directory | 1 |
| the temporary file is left world-readable | 1 |
| the read is sized by seeking rather than read in chunks | 1 |
| `PRIVATE` widens the file instead of leaving it alone | 1 |
| `DEFAULT` uses a hardcoded `0644` rather than asking | 3 |
| `PRESERVE` stats the destination and discards the answer | 2 |
| the probe file is left behind | 1 |
| the probe follows a name that is already there | 1 |
| a probe that could not be made is ignored | 1 |
| a failure to settle the permissions does not fail the commit | 1 |
| an unrecognised `GCU_File_Perms` is accepted rather than refused | 1 |

Two of those failed nothing on the first attempt.  There was no test in which
`temp_create` succeeded and the *rename* then failed - the only path on which
`commit` has litter of its own to remove - and no test that could tell where
`write_atomic` put its temporary file.  Both gaps are now closed by the two
tests named in sections 4 and 5, and both were found by the sabotage rather
than by reading the code.

The permissions work repeated that.  The first test written for "the
permissions could not be settled" made the directory unwritable, which also
breaks the `rename`, so the call failed either way and the test passed against
a build that ignored the failure entirely.  It was replaced by one that blocks
only the probe, by planting a symbolic link at the name the probe will take -
which reaches the branch in isolation and pins the `O_EXCL` at the same time.

Two mutations are recorded here as *not* caught, rather than left to look like
coverage:

- **Applying the permissions after the rename instead of before.**  The
  finished file has the same mode either way; what differs is a window in
  which the destination is readable with the wrong permissions.  A
  single-threaded test cannot observe it, and the mutation survives every
  assertion in the suite.  It is section 8's reasoning, not a tested property.
- **Ignoring what `fchmod` returns.**  It cannot be made to fail for a
  descriptor the test owns, and a read-only filesystem is not something a test
  may assume it can arrange.  The neighbouring failure - a probe that cannot
  be created - *is* covered, so the reporting path around it is exercised even
  though this one call's return value is not.

### What the later work's sabotage found

The directory and handle work was checked the same way, and two of the findings
were about the *harness* rather than the code - which is the failure mode this
technique has, and worth recording.

| Sabotage | Tests failing |
| --- | --- |
| `create_all` treats an existing non-directory as success | 1 |
| `read` reports `.` and `..` like any other entry | 3 |
| `open` leaves the handle untouched when it fails | a crash |

Breaking the `.` and `..` skip first appeared to be caught by **nothing**.  It
was not:  the fixture wiped its scratch tree by walking it with
`gcu_dir_read()`, so removing that skip sent every teardown into infinite
recursion and the suite hung.  A hung run names no failing tests at all, which
reads on the terminal exactly like a mutation that survived.  Two things came
out of it.  The fixture now walks with `opendir` directly, so it no longer
depends on the behaviour it is checking; and the sabotage script now imposes a
timeout and reads the exit status, because a count of zero failures means
nothing until you know the run finished.

One mutation is recorded as **not caught**:  starting `create_all`'s walk at
zero rather than past the root.  On POSIX the only root is `/`, and starting at
zero merely produces an empty first component that the loop skips anyway, so
the two are indistinguishable here.  It matters on Windows, where the root of
`C:\x` is three characters and starting at zero would try to create `C:` as a
directory.  The test that looked like it covered this did not, and has been
renamed to say what it actually checks.

The matcher's three defects were found by its own tests rather than by
sabotage, and one of them was a genuine out-of-bounds read:  extending a `*`
did not check that there was a character left to extend over, so a pattern that
ran out of path walked off the end of it.  The test that exposed it was written
to check that matching cannot be made exponential, which is a reminder that a
hostile-input test earns its keep twice.
