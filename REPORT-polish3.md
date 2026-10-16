# 📚 Полиране кръг 3 — `lwext4fs-mac` (timestamps · errno mapping · statfs)

_Отчет: 2026-10-16 · автор: CecoBot (subagent lwext4fs-mac-polish3)_

Всичко пипано е **само** под `/Users/ceco/lwext4-spike/`. Реални устройства
(`/dev/disk0s3` и др.) **не са докосвани**; тестовете ползват throwaway image-и.

## 1. 🎯 Обхват
1. **Timestamps при запис** — lwext4 не поддържа mtime/ctime; фронт-ендът ги
   вдига след успешен `write`/`truncate`/`ftruncate` (само в RW режим).
2. **Точно errno mapping** — един helper `lw_errno()` от `ext4_errno.h` →
   POSIX (negated), ползван навсякъде вместо generic `-1`/`EIO`.
3. **Точен `statfs`** — реални бройки от суперблока + изваждане на
   root-резервираните блокове, за да съвпада `df -h` с native ext4.

## 2. 📝 Променени файлове / функции
Всичко в `lwext4fs-mac/`:

- **`src/ops.c`**
  - `lw_errno(int r)` — **нов** централен мапер (таблица долу). Ползва се във
    всички callbacks вместо `-r` / `r == EOK ? 0 : -r`.
  - `lw_touch_write(const char *path)` — **нов**; вдига mtime+ctime на „сега“,
    гейтнато на `g_read_write` (RO guard).
  - `lw_dir_is_empty(path, &empty)` — **нов**; проверка за празна директория.
  - `lw_write` — след успешен запис вика `lw_touch_write()`; ако
    `wr < size` и rc==EOK → връща **ENOSPC** (компенсация за lwext4 бъг, §4).
  - `lw_truncate`, `lw_ftruncate` — викат `lw_touch_write()` след успех.
  - `lw_rmdir` — pre-check за празнота → **ENOTEMPTY** (POSIX rmdir семантика,
    §4).
  - `lw_statfs` — изважда `reserved_blocks_count` (lo + hi при 64BIT) от
    `f_bavail`; добавен `f_favail`; map-ва грешка през `lw_errno`.
  - `lw_readdir` — `ext4_dir_open` грешка вече се map-ва (не hardcode ENOENT).
  - `lw_xattr_errno` — пре-дефиниран върху `lw_errno` (пази ENODATA→ENOATTR).
  - Header док-блокът — нови секции „Error mapping“ и „Timestamps“.
- **`README.md`** — нови функции, таблицата с errno-та, бележки за statfs.
- **`test_polish.sh`** — **нов** тест (18 проверки) за трите фийчъра.
- **`lwext4/` — НЕ е пипан** (няма промени по библиотеката → без rebuild).

## 3. 🔑 `lw_errno()` — errno mapping table
Този build е с `CONFIG_HAVE_OWN_ERRNO == 0` ⇒ lwext4 връща **host `<errno.h>`**
стойности ⇒ мапингът е идентитет; разписан е явно за одитируемост.

| lwext4 rc | връща към FUSE | macOS № |
|---|---|---|
| EOK | 0 | 0 |
| EPERM | -EPERM | 1 |
| ENOENT | -ENOENT | 2 |
| EIO | -EIO | 5 |
| ENXIO | -ENXIO | 6 |
| E2BIG | -E2BIG | 7 |
| ENOMEM | -ENOMEM | 12 |
| EACCES | -EACCES | 13 |
| EFAULT | -EFAULT | 14 |
| EEXIST | -EEXIST | 17 |
| ENODEV | -ENODEV | 19 |
| ENOTDIR | -ENOTDIR | 20 |
| EISDIR | -EISDIR | 21 |
| EINVAL | -EINVAL | 22 |
| EFBIG | -EFBIG | 27 |
| ENOSPC | -ENOSPC | 28 |
| EROFS | -EROFS | 30 |
| EMLINK | -EMLINK | 31 |
| ERANGE | -ERANGE | 34 |
| ENOTEMPTY | -ENOTEMPTY | 66 |
| ENAMETOOLONG | -ENAMETOOLONG | 63 |
| EMFILE | -EMFILE | 24 |
| ENFILE | -ENFILE | 23 |
| ENOSYS | -ENOSYS | 78 |
| EXDEV | -EXDEV | 18 |
| EOVERFLOW | -EOVERFLOW | 84 |
| ELOOP | -ELOOP | 62 |
| ENODATA | -ENODATA (xattr → -ENOATTR) | 96 / 93 |
| ENOTSUP | -ENOTSUP | 45 |
| *(друго)* | -EIO | 5 |

## 4. 🐞 Намерени и решени lwext4 бъгове (компенсирани във фронт-енда)
1. **`ext4_fwrite()` трие собствената си грешка.** В `Finish:` пътя прави
   `r = ext4_fs_put_inode_ref(&ref);` → при пълна файлова система записът се
   връща като **EOK с къс count** (не ENOSPC). Ядрото после дава bare `EIO`.
   **Фикс:** `lw_write()` третира `wr < size` (при EOK) като **ENOSPC**.
2. **`ext4_dir_rm()` е рекурсивна.** НЕ е POSIX `rmdir` — изтрива цялото
   поддърво и връща EOK! **Фикс:** `lw_rmdir()` първо проверява за празнота и
   връща **ENOTEMPTY**; `ext4_dir_rm()` се вика само след това (бонус: защита
   срещу случайно рекурсивно изтриване).

## 5. 🧪 Резултати

### `test_polish.sh` — SUMMARY: **PASS=18 FAIL=0**
- **mtime:** create mtime=1792141359 → append(+1s) 1792141360 → truncate(+1s)
  1792141361. Non-epoch ✅, напредва ✅, skew 0s ✅; plain read не пипа mtime ✅.
- **errno (точни):** ENOENT=2 · EISDIR=21 · EEXIST=17 (mkdir+O_EXCL) ·
  ENOTEMPTY=66 · ENOSPC=28 ✅
- **statfs:** 256Mi размер; Avail 211Mi < Size (резервът изваден);
  след 100 MiB запис Avail 215968K → 113568K ✅
- `e2fsck -fn` rc=0 на всеки от трите сценария.

### Регресии (всички rc=0)
- `test.sh` → **E2FSCK_RC=0** ✅
- `test_features.sh` → **PASS=33 FAIL=0**, fsck rc=0 ✅
- `test_hardening.sh` → 8 паралелни writer-а ОК, **E2FSCK_RC=0** ✅
- `test_stress.sh` → ALL_WORKERS_OK=1, **E2FSCK_RC=0** ✅

### Build (`-Wall -Wextra`) — **0 warnings**

```
cc -O2 -g -Wall -Wextra ... -c src/main.c -o src/main.o
cc -O2 -g -Wall -Wextra ... -c src/ops.c  -o src/ops.o
cc src/main.o src/ops.o .../liblwext4.a .../libblockdev.a -lfuse -pthread -lpthread -o lwext4fs-mac
```

## 6. ⏭️ Решение за atime при read
**Не** се вдига atime при `read`: ще добави inode-запис на **всеки** read
(сериозен overhead) и би нарушил read-only гаранцията при чисти четения
(lwext4 е монтиран rw вътрешно). `utime`/`utimens` остават за явен контрол.
