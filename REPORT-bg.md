# 📚 Систематизация — „EXT4 read-write за macOS" (проект `lwext4fs-mac`)

_Отчет: 2026-10-16 · актуализиран след всеки кръг полиране_

## 1. 🎯 Цел
Да **четем и пишем** Linux (ext4) дяла на Fedora **от macOS** — защото нативните/съществуващите инструменти не можеха:
`ext4fuse` (RO по дизайн) · `ExtendFS` (FSKit, RO) · `Paragon extFS` (trial изтекъл).

## 2. 🛤️ Пътят (хронология)
1. **Проучване** — защо записът на ext4 е труден (журнал/jbd2, metadata checksums, алокатори, crash-consistency).
2. **Spike** — компилирахме **lwext4** на macOS → пълен ext4 R/W; открихме и решихме **единствения блокер** (`metadata_csum_seed`) с 1-редов патч; `e2fsck` чист.
3. **Изграждане** — `lwext4fs-mac`: ext4 R/W FS през **macFUSE + lwext4**.
4. **Полиране кръг 1** — многонишковост, file-handle кеш, timestamps при create.
5. **Полиране кръг 2** — read-only по подразбиране, symlink/readlink/link, xattr.
6. **Полиране кръг 3** — `mtime`/`ctime` при запис, прецизно error mapping, `statfs`.

> Защо не FSKit: иска ограничен Apple entitlement (`com.apple.developer.fskit.fsmodule`). macFUSE заобикаля това.

## 3. 🏗️ Архитектура
- **macFUSE** (FUSE 2 API) = мостът VFS↔userspace; ние реализираме `fuse_operations`.
- Всяка FUSE операция → **lwext4** C API (пълен ext4 стек: block cache, extents, journal, metadata_csum, dir index).
- Блоково устройство: lwext4 `file_dev` върху файл/устройство.
- **Заключване:** рекурсивен pthread мютекс (на op ниво + регистриран в lwext4).
- **ino⇄path** таблица (lwext4 е path-базиран).

## 4. ✅ Реализирани функции
- Пълен набор FUSE ops: `getattr, readdir, open, create, read, write, flush, fsync, release, mkdir, unlink, rmdir, rename, truncate/ftruncate, chmod, chown, utime/utimens, statfs, access, mknod, opendir…`
- **symlink / readlink / link** (`ext4_fsymlink` · `ext4_readlink` · `ext4_flink`)
- **xattr** — само `user.*`; `com.apple/trusted/system/security` → `-ENOTSUP` (get → `-ENOATTR`)
- **Read-only по подразбиране** + `-o rw` (kernel `-o ro` + op-guard `-EROFS`)
- **Многонишковост** + **file-handle кеш** + **timestamps при create**
- **`mtime`/`ctime` при запис** (вдигат се след `write`/`truncate`) · `atime` — НЕ при `read` (по дизайн, вж. §7)
- **Прецизно error mapping** (`lw_errno()`, 31 места) · **точен `statfs`**

## 5. 🔧 Открити и решени бъгове (3 в lwext4)
1. `include/ext4_types.h` — `metadata_csum_seed` (`0x2000`) → игнориран (коректно; seed = `crc32c(UUID)`).
2. `src/ext4_xattr.c` — `ext4_xattr_remove()` ползваше неинициализиран `block_finder.s` вместо `ibody_finder.s` → `removexattr` беше **тих no-op**. Фикс.
3. **`ext4_fwrite()` изтриваше грешката си** → при **пълен диск** връщаше „успех" с кратък запис, ядрото показваше голо `EIO`. Обезвредено във front-end → коректно **`ENOSPC`**.
4. **`ext4_dir_rm()` е РЕКУРСИВЕН!** → `rmdir` върху непразна директория **триеше цялото дърво** с „успех". Обезвредено: front-end проверява празнота → **`ENOTEMPTY`**.

_(№3 и №4 са компенсирани във front-end; lwext4 lib не е пипан в кръг 3.)_

## 6. 🧪 Верификация (доказано)
- Монтиране → запис/четене → `umount` → **`e2fsck -fn` rc=0** (вкл. точния Fedora формат `incompat=0x22C2 / ro=0x046B`)
- **8 паралелни writer-а** → байт-идентични · **900 смесени операции** → OK
- RO → запис отказан (`EROFS`); `-o rw` → OK · symlink `readlink`→целта · hardlink `nlink=2`
- xattr round-trip; `com.apple.*` не се пази
- `mtime` напредва след append (напр. `…544 → …545`); четенето не го пипа
- **Точни errno:** `ENOENT=2` · `EISDIR=21` · `EEXIST=17` · `ENOTEMPTY=66` · `ENOSPC=28`
- `statfs`: `Avail < Size` (reserved изваден); намалява след голям запис
- **Всички регресионни тестове минават** · build с **0 warnings**

## 7. ⚠️ Ограничения
- **I/O сериен** — един глобален mutex (thread-safe, но не паралелен; такова е lwext4)
- `atime` не се обновява при `read` (иначе inode-write на всеки прочит; `utimens` го задава изрично)
- Синтетични ino (hardlink не споделя ino; `nlink` е верен)
- symlink цел > 1 блок → `EINVAL`
- xattr само `user.*`; флагове (`XATTR_CREATE/REPLACE`) и позиция (resource forks) игнорирани; ACL xattr няма high-level API
- NFD/UTF-8 имена не се нормализират
- Няма `flock`/`ioctl`/`poll`/`fallocate`

## 8. 🚨 Рискове + предпазни мерки
| # | Риск | Т. |
|---|---|---|
| R1 | lwext4 не е kernel-grade → бъг = повреда | 🔴 |
| R2 | Двоен запис (Fedora активна/хибернирала) | 🔴 |
| R3 | Няма crash-atomicity | 🟠 |
| R4 | Неподдържани features мълчаливо сгрешени | 🟠 |
| R5 | Остатъчни upstream бъгове в lwext4 | 🟡 |

**Мерки:** само при изключена Fedora · пресен бекъп · baseline `e2fsck -fn` преди/след · първо четене, после писане · за обмен → споделен exFAT.

## 9. 📦 Артефакти + употреба
`/Users/ceco/lwext4-spike/lwext4fs-mac/`: `lwext4fs-mac` (binary), `src/{main.c,ops.c,ops.h}`, `Makefile`, `test*.sh`, `README.md`, `REPORT-bg.md`
```
sudo ./lwext4fs-mac /dev/disk0s3 /Volumes/Fedora        # read-only (по подразбиране)
sudo ./lwext4fs-mac /dev/disk0s3 /Volumes/Fedora -o rw  # със запис
```
Пълна документация: `memory/knowledge-base/macos-ext4-write-spike.md`.

## 10. ▶️ Следващи стъпки
1. Пробен запис на **реалния дял** (`disk0s3`) — **с бекъп**, само потребителски файлове
2. Истински **паралелен I/O** (иска патч в lwext4) + crash-safety
3. Пакетиране (helper/app за лесен mount)
