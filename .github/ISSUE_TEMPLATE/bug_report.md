---
name: Bug report
about: A query misbehaves, crashes, or returns the wrong result
title: "[bug] "
labels: bug
---

**What happened**
A clear description of the bug.

**Minimal reproducer**
The smallest SQL that shows the problem:

```sql
CREATE TABLE t (...);
INSERT INTO t VALUES (...);
SELECT ...;   -- this returns X but should return Y
```

**Expected result**

**Actual result**
(Paste output, error message, or crash.)

**Environment**
- OS:
- Compiler + version (e.g. GCC 13, Clang 17, MSVC 19.3):
- LiteQuery version / commit:
