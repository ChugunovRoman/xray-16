# Sentry integration (OpenXRay / xray-16)

## CMake (Linux, macOS, …)

```bash
cmake -B build -S . -DWITH_SENTRY=ON
cmake --build build
```

**Linux:** sentry-native uses the **curl** transport and CMake’s `find_package(CURL COMPONENTS AsynchDNS)`. Install the distro **libcurl development** package (e.g. Debian/Ubuntu **`libcurl4-openssl-dev`**, Fedora **`libcurl-devel`**, Alpine **`curl-dev`**), otherwise configuration fails with `CURL: Required feature AsynchDNS is not found`.

`Externals/sentry-native` must exist with **recursive submodules** (CMake does not auto-fetch sentry-native).

**CI:** `misc/ci/ensure_sentry_native.sh` runs on GitHub Actions after checkout and clones [sentry-native](https://github.com/getsentry/sentry-native) `0.7.17` if the tree is missing (your fork may not list it in `.gitmodules` yet). It then runs `misc/ci/apply_crashpad_gcc_fix.py` so **GCC** can compile Crashpad Linux snapshot code (template/`!` parse issues with **unity builds**).

**Рекомендация для репозитория:** добавить submodule и закоммитить, чтобы `git clone --recursive` сразу тянул дерево:

```bash
git submodule add -b 0.7.17 https://github.com/getsentry/sentry-native.git Externals/sentry-native
git -C Externals/sentry-native submodule update --init --recursive
git add .gitmodules Externals/sentry-native
git commit -m "Add sentry-native submodule"
```

## Visual Studio / MSBuild

1. Ensure `Externals/sentry-native` exists (clone tag `0.7.17` from [getsentry/sentry-native](https://github.com/getsentry/sentry-native)), then run:
   `cd Externals/sentry-native && git submodule update --init --recursive`
2. Run `misc/windows/build_sentry_native.ps1` or `misc/windows/build_sentry_native.sh` for each needed platform/config (see `sdk/sentry/README.md`).
3. Enable Sentry for the solution, e.g. in `src/xray.user.props`:

```xml
<Project>
  <PropertyGroup>
    <UseSentry>true</UseSentry>
  </PropertyGroup>
</Project>
```

BugTrap is disabled when `USE_SENTRY` is defined (`UseSentry=true`).

## GitHub Actions and cibuild.yml

Сборка с Sentry уже завязана на workflow:

- **Windows (MSBuild):** после checkout вызывается `write_xr_sentry_embed.sh`, кэшируется `sdk/sentry`, при промахе кэша — `build_sentry_native.ps1` для текущих `matrix.Platform` / `matrix.Configuration`, затем `msbuild ... /p:UseSentry=true`.
- **CMake (Ubuntu, macOS, Alpine, Fedora):** в строку CMake добавлено `-DWITH_SENTRY=ON`.

**Обязательно в репозитории:** каталог `Externals/sentry-native` с инициализированными подмодулями (в CI `submodules: recursive`). Если этого нет — добавьте [sentry-native](https://github.com/getsentry/sentry-native) как submodule или закоммитьте дерево с `git submodule update --init --recursive`.

**Секрет:** `SENTRY_DSN` — подставляется в `xrSentry_embed.h` перед сборкой (может быть пустым на PR из форков).

**BSD job** в `cibuild.yml` по-прежнему без `WITH_SENTRY` (при необходимости добавьте `-DWITH_SENTRY=ON` в шаги CMake в VM самостоятельно).

## DSN: вшить в бинарник (для всех пользователей)

Переменная **`SENTRY_DSN`** по-прежнему **переопределяет** встроенное значение (удобно для QA и отладки).

**GitHub Actions:** см. раздел [GitHub Actions and cibuild.yml](#github-actions-and-cibuildyml) выше — там же про секрет **`SENTRY_DSN`** и [`misc/ci/write_xr_sentry_embed.sh`](../../misc/ci/write_xr_sentry_embed.sh).

**Visual Studio / MSBuild:** отредактируйте [`src/xrCore/Debug/xrSentry_embed.h`](../../src/xrCore/Debug/xrSentry_embed.h) — задайте:

`#define XRAY_SENTRY_DEFAULT_DSN_STR "https://…@….ingest….sentry.io/…"`

**CMake:** при конфигурации задайте кэш-переменную (перекрывает пустой `xrSentry_embed.h` для этой сборки):

```bash
cmake -B build -S . -DWITH_SENTRY=ON -DXRAY_SENTRY_EMBED_DSN="https://key@o….ingest….sentry.io/123"
```

Строка DSN видна в исполняемом файле; при утечке/абьюзе ротируйте ключ в настройках проекта Sentry.

## Symbol upload (sentry-cli)

After building, upload PDBs / debug files so Sentry can symbolicate minidumps:

```powershell
# Windows example — set org, project, auth token in env or login via sentry-cli
sentry-cli login
sentry-cli debug-files upload --org YOUR_ORG --project YOUR_PROJECT ..\bin\x64\Release\*.pdb
```

Use the **same** `SENTRY_RELEASE` / release string as in the client (`SENTRY_RELEASE` env or git-based default in `xrSentry.cpp`).

See also: `upload_debug_files.ps1` (Windows) and `upload_debug_files.sh` (bash / Git Bash / WSL) in this folder.

### Автозагрузка PDB из GitHub Actions (Windows Release x64)

**Вариант A — без кликов после пуша:** в настройках репозитория задай секреты:

| Secret | Назначение |
|--------|------------|
| `SENTRY_AUTH_TOKEN` | Auth token Sentry с правом загрузки debug files |
| `SENTRY_ORG` | slug организации (из URL Sentry) |
| `SENTRY_PROJECT` | slug проекта |

После каждого успешного шага **Prepare artifacts** для матрицы **Release + x64** в `cibuild.yml` PDB из `res\bin` уйдут в Sentry. Если секретов нет — шаг тихо пропускается.

**Вариант B — один клик по готовому прогону:** workflow [**Sentry symbols from CI run**](../../.github/workflows/sentry-symbols-from-run.yml) (`workflow_dispatch`):

1. Открой нужный зелёный прогон **Build** → скопируй **run id** из URL (`.../actions/runs/<RUN_ID>`).
2. **Actions** → **Sentry symbols from CI run** → **Run workflow** → вставь `run_id`.
3. Те же секреты `SENTRY_AUTH_TOKEN`, `SENTRY_ORG`, `SENTRY_PROJECT` (или org/project можно ввести в форме workflow, если секреты пустые).

Скачивается артефакт вида `Symbols.Release x64 (github-…).7z`, распаковывается, вызывается `sentry-cli debug-files upload`.

Локальный путь без CI: старый ручной workflow [**Sentry symbols (manual)**](../../.github/workflows/sentry-symbols.yml) (нужна папка с PDB в checkout).

## Verification checklist

- [ ] Event appears in Sentry for `-sentry_test_av_crash` with DSN set.
- [ ] `crashpad_handler.exe` is next to `xrEngine.exe` in the output directory.
- [ ] No BugTrap dialog when `UseSentry=true`.
- [ ] Offline: crash with network off, then restart online — queued event uploads.
