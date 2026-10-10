# Phase 10 — Settings page, documentation and final verification

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Every new key is editable on the GUI settings page (global keys as dotted paths that override one leaf at a time), every feature is documented on the website and in the release notes, parser throughput is shown not to regress, and the whole branch is verified and reviewed, ready for the owner to open the PR that closes #1010.

**Architecture:** `settings.yml` keeps its flat in-memory model (`GuiManagedSettings::globalOverrides`, a map from key to YAML scalar) but the key becomes a *path* (`gutter.line_numbers`). `emitGuiSettingsYaml` writes a path as nested YAML and `loadGuiSettingsFile` flattens it back, so `FileGuiConfigStore` and `SettingsController` keep their shapes; `mergeGuiManagedSideFiles` applies a section through its existing loader, which writes only the children present — that is what makes one override leave its siblings alone. New settings-page rows are table rows (`GlobalFieldDescriptor`, `ProfileFieldGroup`, a derived-colour-slot table) whose enum options come from the loader's `ConfigEnum` tables; a data-driven test drives every global row through the real store and reload, so a row without a loader line cannot ship again. Documentation is mostly generated (`contour documentation …` from `ConfigDocumentation.hpp` and `actionCatalog()`); this phase verifies the generated parts and writes the hand-written pages.

**Tech Stack:** C++23, yaml-cpp, Qt 6 / QML (`Qt6::Test` harness in `contour_gui_test`), Catch2, mkdocs (Markdown), AppStream `metainfo.xml`, valgrind/callgrind and gcovr in WSL (Ubuntu 24.04, clang-22).

**Spec:** [`docs/drafts/semantic-blocks.md`](../semantic-blocks.md) — read §11 (keys and the settings page), §13.1 (performance), §15 step 10 (docs, release notes) and §16.1 (last item: nested YAML leaves). Global constraints, build commands and the Interface Contract: [README](README.md#global-constraints).

---

## What the code says about nested overrides (decides the design)

`YAMLConfigReader::loadFromEntry` for a `ConfigEntry<T>` forwards into the **live** value
(`loadFromEntry(doc, entry, where.value())`, `src/contour/config/Config.hpp:1445-1450`; the node
overload at `:1426-1439` adds a `try/catch`), and a section loader assigns only the children that are
present (`FoldingConfig`, `src/contour/config/Config.cpp:2193-2203`; scalars at `Config.hpp:1454-1460`
assign only `if (child)`). So **loading a partial map into an already-populated struct keeps its
siblings** — no per-leaf YAML synthesis is needed. What is missing today:

1. `loadGuiSettingsFile` (`Config.cpp:4121-4132`) keeps only top-level *scalars*: a nested section in
   `settings.yml` is dropped on load and therefore deleted on the next save.
2. `emitGuiSettingsYaml` (`Config.cpp:4064-4074`) would write a dotted key as a literal top-level key
   `gutter.line_numbers:`, which no loader looks up.
3. `mergeGuiManagedSideFiles` (`Config.cpp:838-865`) has no line for any section.

Task 10.1 pins the reader behaviour with a test and fixes the three gaps. Reading the same function also
showed a pre-existing defect: the settings page offers `grapheme_clustering` and `text_scaling_method`
(`SettingsController.cpp:1058-1076`) but the merge has no loader line for either, so their overrides
are written and never applied. Task 10.2 adds a test over every global row that catches that class of
bug, and fixes it.

---

### Task 10.1: settings.yml stores a dotted override as a nested section, applied leaf by leaf

**Files:**
- Modify: `src/contour/config/Config.hpp` (`GuiManagedSettings::globalOverrides` doc, `:512-516`)
- Modify: `src/contour/config/Config.cpp` (`mergeGuiManagedSideFiles` `:838-865`, `emitGuiSettingsYaml` `:4064-4074`, `loadGuiSettingsFile` `:4121-4132`)
- Test: `src/contour/config/Config_test.cpp` (append before the `// }}}` that closes the GUI side-file block, after `TEST_CASE("Config: FileGuiConfigStore writes and removes side files the loader picks up"`)

**Interfaces:**
- Consumes: `Config::folding` (`ConfigEntry<FoldingConfig, documentation::Folding>`, existing); `Config::gutter` (`ConfigEntry<GutterConfig, …>`, C9, phase 4); `Config::commandBlocks` (`ConfigEntry<CommandBlocksConfig, …>`, C9, phases 1/5); their section loaders `void YAMLConfigReader::loadFromEntry(YAML::Node const&, std::string const&, GutterConfig&)` and `(…, CommandBlocksConfig&)`; `template <…> void YAMLConfigReader::loadFromEntry(YAML::Node const& node, std::string const& entry, ConfigEntry<T, …>& where)` (`Config.hpp:1426`).
- Produces: no new public symbol. Changed semantics of `std::string emitGuiSettingsYaml(GuiManagedSettings const&)` and `std::expected<GuiManagedSettings, std::string> loadGuiSettingsFile(std::filesystem::path const&)`: a `globalOverrides` key is a dotted path, stored nested in `settings.yml`. `GuiConfigStore` / `FileGuiConfigStore` are unchanged (`GuiConfigStore.cpp:54-58` already routes through `emitGuiSettingsYaml`), and `SettingsController` already merges with the siblings in the file because it copies `_config().guiManagedSettings` — now complete — before setting one key (`SettingsController.cpp:1546-1547`).

- [ ] **Step 0: Record the phase start and confirm the consumed names**

Run (Git Bash, worktree root):
```bash
mkdir -p out && git rev-parse HEAD > out/phase10-start.sha && cat out/phase10-start.sha
grep -n "ConfigEntry<FoldingConfig\|ConfigEntry<GutterConfig\|ConfigEntry<CommandBlocksConfig" src/contour/config/Config.hpp
grep -n "std::string const& entry, GutterConfig& where\|std::string const& entry, CommandBlocksConfig& where" src/contour/config/Config.hpp
grep -n -A8 "std::string const& entry, GutterConfig& where)\|CommandBlocksConfig& where)\|std::string const& entry, FoldingConfig& where)" src/contour/config/Config.cpp | grep "where = "
```
Expected: one SHA; three member lines named `folding`, `gutter`, `commandBlocks`; two loader declarations; **no** output from the last grep. If a member carries another name, use the real name everywhere in this phase. If a member or loader is missing, stop and report (the producing phase did not meet C9). If the last grep prints a line, that loader resets the whole struct before reading — delete that assignment so the loader writes only the children present (the `FoldingConfig` shape, `Config.cpp:2193-2203`); it is exactly what would make one override reset its siblings.

- [ ] **Step 1: Write the failing tests**

Add, where absent, `#include <vtbackend/screen/Gutter.hpp>` (Task 4.11 already added it), `#include <yaml-cpp/yaml.h>`, `#include <map>` and `#include <optional>` to the include block of `Config_test.cpp` (keep each group sorted). Append:

```cpp
TEST_CASE("Config: settings.yml writes a dotted override as a nested section and reads it back flat",
          "[config][gui]")
{
    QTemporaryDir const dir;
    auto const path = std::filesystem::path(dir.path().toStdString()) / "settings.yml";
    auto const settings = contour::config::GuiManagedSettings {
        .defaultProfile = "work",
        .globalOverrides = { { "folding.enabled", "false" },
                             { "folding.show_markers", "true" },
                             { "reflow_on_resize", "false" } },
    };
    auto const yaml = contour::config::emitGuiSettingsYaml(settings);
    INFO("settings.yml:\n" << yaml);
    {
        auto out = std::ofstream(path);
        out << yaml;
    }

    // Spelled the way contour.yml spells it. A literal `folding.enabled:` key is a key no section
    // loader ever looks up, so the override would save and then silently do nothing.
    auto const doc = YAML::Load(yaml);
    REQUIRE(doc["folding"].IsMap());
    CHECK(doc["folding"]["enabled"].as<std::string>() == "false");
    CHECK(doc["folding"]["show_markers"].as<std::string>() == "true");
    CHECK(doc["reflow_on_resize"].as<std::string>() == "false");
    CHECK_FALSE(doc["folding.enabled"].IsDefined());

    auto const loaded = contour::config::loadGuiSettingsFile(path);
    REQUIRE(loaded.has_value());
    CHECK(loaded->defaultProfile == std::optional<std::string> { "work" });
    CHECK(loaded->globalOverrides == settings.globalOverrides);

    SECTION("a stale flat value where a section is needed gives way to the section")
    {
        auto const mixed = contour::config::GuiManagedSettings {
            .defaultProfile = std::nullopt,
            .globalOverrides = { { "folding", "true" }, { "folding.enabled", "false" } },
        };
        auto const mixedDoc = YAML::Load(contour::config::emitGuiSettingsYaml(mixed));
        REQUIRE(mixedDoc["folding"].IsMap());
        CHECK(mixedDoc["folding"]["enabled"].as<std::string>() == "false");
    }
}

TEST_CASE("Config: a nested settings.yml override replaces one leaf and keeps its siblings", "[config][gui]")
{
    // The settings page writes only the leaves the user changed. Each section loader writes only the
    // children present into a struct contour.yml already filled (Config.hpp's ConfigEntry overload loads
    // into the live value), so what the GUI did not touch keeps contour.yml's value -- not the default.
    QTemporaryDir dir;
    writeSideFile(dir, "settings.yml", R"(
folding:
    enabled: false
gutter:
    line_numbers: relative
command_blocks:
    pager: most
)");
    auto const config = loadFromYaml(dir, R"(
default_profile: main
folding:
    show_markers: false
    auto_collapse_on_new_command: true
gutter:
    timestamps: true
    timestamp_format: "%H:%M"
    line_number_width: 8
command_blocks:
    max_records: 250
)");

    // What the GUI set wins ...
    CHECK(config.folding.value().enabled == false);
    CHECK(config.gutter.value().lineNumbers == vtbackend::LineNumberMode::Relative);
    CHECK(config.commandBlocks.value().pager == "most");

    // ... and every sibling it did not set keeps what contour.yml said.
    CHECK(config.folding.value().showMarkers == false);
    CHECK(config.folding.value().autoCollapseOnNewCommand == true);
    CHECK(config.gutter.value().timestamps == true);
    CHECK(config.gutter.value().timestampFormat == "%H:%M");
    CHECK(config.gutter.value().lineNumberWidth == 8);
    CHECK(config.commandBlocks.value().maxRecords == size_t { 250 });
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run:
```bash
cmake --build --preset clangcl-debug --target contour_gui_test
out/build/clangcl-debug/bin/contour_gui_test.exe "Config: settings.yml writes a dotted override as a nested section and reads it back flat"
out/build/clangcl-debug/bin/contour_gui_test.exe "Config: a nested settings.yml override replaces one leaf and keeps its siblings"
```
Expected: both FAIL. The first on `REQUIRE(doc["folding"].IsMap())` (the emitter writes a flat `folding.enabled:` key). The second on the three "GUI wins" checks (`enabled` stays `true`, `lineNumbers` stays `Off`, `pager` keeps its default): the loader drops the nested maps and the merge has no section lines. The seven sibling checks already PASS — that is the reader behaviour the design relies on. (If `contour_gui_test.exe` exits `0xC0000135`, apply the Qt `PATH` workaround in the README.)

- [ ] **Step 3: Document the key-path semantics**

In `src/contour/config/Config.hpp`, replace the `globalOverrides` doc comment inside `struct GuiManagedSettings`:

Before:
```cpp
    /// GUI-set global overrides, keyed by the contour.yml top-level key, valued as the YAML scalar to
    /// write (e.g. "reflow_on_resize" -> "false"). Present here == overridden by the GUI; the on-load
    /// merge re-applies each through the same per-key loader contour.yml uses, so the value is typed
    /// correctly. Absent keys defer to contour.yml.
    std::map<std::string, std::string> globalOverrides;
```
After:
```cpp
    /// GUI-set global overrides, keyed by the contour.yml key PATH and valued as the YAML scalar to
    /// write: a top-level key ("reflow_on_resize" -> "false") or a dotted path to one leaf of a section
    /// ("gutter.line_numbers" -> "relative"). Present here == overridden by the GUI; absent keys defer
    /// to contour.yml.
    ///
    /// settings.yml spells a dotted path as nested YAML (`gutter:` / `  line_numbers: relative`), the
    /// way contour.yml does, and loadGuiSettingsFile() flattens it back. The on-load merge re-applies
    /// each override through the loader contour.yml uses; a section's loader writes only the children
    /// present, so one overridden leaf leaves its siblings at their contour.yml values. A sequence in
    /// settings.yml is not an override the page can write, and is not kept.
    std::map<std::string, std::string> globalOverrides;
```

- [ ] **Step 4: Write nested, read flat**

In `src/contour/config/Config.cpp`, replace `emitGuiSettingsYaml` (`:4064-4074`):

Before:
```cpp
std::string emitGuiSettingsYaml(GuiManagedSettings const& settings)
{
    YAML::Emitter out;
    out << YAML::BeginMap;
    if (settings.defaultProfile)
        out << YAML::Key << "default_profile" << YAML::Value << *settings.defaultProfile;
    for (auto const& [key, value]: settings.globalOverrides)
        out << YAML::Key << key << YAML::Value << value;
    out << YAML::EndMap;
    return std::string { out.c_str() } + '\n';
}
```
After:
```cpp
namespace
{
    /// Writes @p value at the dotted @p path below @p node ("gutter.line_numbers" becomes
    /// `gutter: { line_numbers: value }`), creating each section on the way.
    ///
    /// @p node is taken by value on purpose: a YAML::Node copy shares its data, so writes reach the
    /// caller's tree, whereas ASSIGNING one node to another would rebind the caller's node instead.
    /// A scalar standing where a section is needed (a stale flat override) gives way to the section,
    /// because no loader reads a key that is both.
    /// @param node  The map to write into.
    /// @param path  The dotted key path below @p node.
    /// @param value The YAML scalar text to store at @p path.
    void insertOverride(YAML::Node node, std::string_view path, std::string const& value)
    {
        auto const dot = path.find('.');
        auto const head = std::string(path.substr(0, dot));
        if (dot == std::string_view::npos)
        {
            node[head] = value;
            return;
        }
        if (auto const existing = node[head]; existing.IsDefined() && !existing.IsMap())
            node[head] = YAML::Node(YAML::NodeType::Map);
        insertOverride(node[head], path.substr(dot + 1), value);
    }

    /// Collects every scalar below @p node into @p out, keyed by its dotted path from @p prefix.
    /// @param node   The map to walk.
    /// @param prefix The path of @p node itself, ending in '.', or empty at the document root.
    /// @param out    Receives one entry per scalar leaf.
    void collectOverrides(YAML::Node const& node,
                          std::string const& prefix,
                          std::map<std::string, std::string>& out)
    {
        for (auto const& entry: node)
        {
            auto const key = prefix + entry.first.as<std::string>();
            if (entry.second.IsScalar())
                out[key] = entry.second.as<std::string>();
            else if (entry.second.IsMap())
                collectOverrides(entry.second, key + '.', out);
        }
    }
} // namespace

std::string emitGuiSettingsYaml(GuiManagedSettings const& settings)
{
    auto root = YAML::Node(YAML::NodeType::Map);
    if (settings.defaultProfile)
        root["default_profile"] = *settings.defaultProfile;
    for (auto const& [key, value]: settings.globalOverrides)
        insertOverride(root, key, value);

    YAML::Emitter out;
    out << root;
    return std::string { out.c_str() } + '\n';
}
```

In `loadGuiSettingsFile`, replace the override loop (`:4124-4132`):

Before:
```cpp
    // Every other top-level scalar is a GUI global override, kept as its YAML scalar text so it can be
    // re-emitted verbatim and re-applied through the typed per-key loader on the next load.
    if (doc.IsMap())
        for (auto const& entry: doc)
        {
            auto const key = entry.first.as<std::string>();
            if (key != "default_profile" && entry.second.IsScalar())
                settings.globalOverrides[key] = entry.second.as<std::string>();
        }
```
After:
```cpp
    // Every other scalar is a GUI global override, kept as its YAML scalar text under its dotted path
    // ("gutter.line_numbers" for `gutter: { line_numbers: ... }`) so it can be re-emitted verbatim and
    // re-applied through the typed loader on the next load.
    if (doc.IsMap())
    {
        collectOverrides(doc, std::string {}, settings.globalOverrides);
        settings.globalOverrides.erase("default_profile");
    }
```

- [ ] **Step 5: Apply the three sections in the merge**

In `mergeGuiManagedSideFiles`, after `overrides.loadFromEntry("early_exit_threshold", config.earlyExitThreshold);` (`:864`), add:
```cpp

            // Sections. settings.yml holds only the leaves the settings page overrode (see
            // emitGuiSettingsYaml), and a section's loader writes only the children present, so an
            // override of gutter.line_numbers leaves a gutter.timestamps from contour.yml as it was.
            // Through the node overload for its try/catch: one malformed leaf is logged rather than
            // aborting the whole configuration load.
            overrides.loadFromEntry(overrides.doc, "folding", config.folding);
            overrides.loadFromEntry(overrides.doc, "gutter", config.gutter);
            overrides.loadFromEntry(overrides.doc, "command_blocks", config.commandBlocks);
```

- [ ] **Step 6: Run the tests to verify they pass**

Run:
```bash
clang-format -i src/contour/config/Config.hpp src/contour/config/Config.cpp src/contour/config/Config_test.cpp
cmake --build --preset clangcl-debug --target contour_gui_test
out/build/clangcl-debug/bin/contour_gui_test.exe "[config]"
out/build/clangcl-debug/bin/contour_gui_test.exe "[settings]"
```
Expected: all PASS, including the existing `Config: GUI settings round-trip through emitGuiSettingsYaml / loadGuiSettingsFile` and `SettingsController: global overrides write settings.yml, apply, and reset` (flat keys are unchanged by the nesting).

- [ ] **Step 7: Commit**

```bash
git add src/contour/config/Config.hpp src/contour/config/Config.cpp src/contour/config/Config_test.cpp
git commit -F - <<'EOF'
config: store dotted settings.yml overrides as nested sections

A settings-page override of one leaf of a section (gutter.line_numbers) is
written as nested YAML, read back flat, and applied through the section's
own loader, which writes only the children present -- so the siblings keep
their contour.yml values. Before, a nested section in settings.yml was
dropped on load and deleted on the next save.

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

---

### Task 10.2: Every global row's override is applied on reload

**Files:**
- Modify: `src/contour/config/Config.cpp` (`mergeGuiManagedSideFiles`, after `:863`)
- Modify: `src/contour/window/SettingsController.cpp` (`globalFieldDescriptors()` doc comment, `:933-934`)
- Test: `src/contour/window/SettingsController_test.cpp` (helper in the anonymous namespace `:29-91`; test before `TEST_CASE("SettingsController: exposes the configured keybindings read-only"`)

**Interfaces:**
- Consumes: `SettingsController::globalFields()`, `setGlobalField(QString const&, QVariant const&)` (`SettingsController.hpp:109,189`); `Config::textScalingMethod`, `Config::graphemeClustering` (`Config.hpp:1293-1295`) and their existing loaders (`Config.cpp:1070-1071`).
- Produces: nothing new; two loader lines.

- [ ] **Step 1: Write the failing test**

Inside the anonymous namespace of `SettingsController_test.cpp`, before `} // namespace` (`:91`), add:
```cpp
/// A value for global-field row @p row that differs from the one it shows now, in the type the row's
/// editor would send.
[[nodiscard]] QVariant differentValue(QVariantMap const& row)
{
    auto const type = row.value("type").toString();
    auto const current = row.value("value");
    if (type == "bool")
        return QVariant(!current.toBool());
    if (type == "int")
        return QVariant(current.toInt() + 1);
    if (type == "double")
        return QVariant(current.toDouble() + 1.0);
    if (type == "enum")
    {
        for (auto const& option: row.value("options").toStringList())
            if (option != current.toString())
                return QVariant(option);
        return current;
    }
    return QVariant(current.toString() == QStringLiteral("x") ? QStringLiteral("y") : QStringLiteral("x"));
}
```
And before `TEST_CASE("SettingsController: exposes the configured keybindings read-only"`:
```cpp
TEST_CASE("SettingsController: every global field's override survives save and reload", "[settings]")
{
    // A global row is two halves in two files: the descriptor here, which writes the override, and a
    // loader line in mergeGuiManagedSideFiles, which applies it. A row whose second half is missing
    // saves without complaint and then does nothing -- after the reload the page shows the old value,
    // with a Reset button for a change that never happened. Driving every row through the real store
    // and the real reload keeps the two halves in step, including for rows added later.
    auto fx = Fixture(BasicConfig);
    auto rows = 0;
    for (auto const& raw: fx.controller->globalFields())
    {
        auto const row = raw.toMap();
        auto const key = row.value("key").toString();
        auto const wanted = differentValue(row);
        INFO("global field: " << key.toStdString() << " -> " << wanted.toString().toStdString());
        REQUIRE(fx.controller->setGlobalField(key, wanted));
        auto const after = rowWithKey(fx.controller->globalFields(), key);
        CHECK(after.value("value") == wanted);
        CHECK(after.value("overridden").toBool());
        ++rows;
    }
    CHECK(rows > 0);
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run:
```bash
cmake --build --preset clangcl-debug --target contour_gui_test
out/build/clangcl-debug/bin/contour_gui_test.exe "SettingsController: every global field's override survives save and reload"
```
Expected: FAIL with exactly two failing `CHECK(after.value("value") == wanted)` — for `grapheme_clustering` and `text_scaling_method`. Every other row passes.

- [ ] **Step 3: Add the two missing loader lines and say why rows need them**

In `mergeGuiManagedSideFiles` (`Config.cpp`), after `overrides.loadFromEntry("ui_font_size", config.uiFontSize);`, add:
```cpp
            overrides.loadFromEntry("text_scaling_method", config.textScalingMethod);
            overrides.loadFromEntry("grapheme_clustering", config.graphemeClustering);
```
In `SettingsController.cpp`, replace the doc comment of `globalFieldDescriptors()`:

Before:
```cpp
    /// The editable global settings. The keys match contour.yml's top-level keys AND the loader lines in
    /// mergeGuiManagedSideFiles, so an override round-trips as the right type.
```
After:
```cpp
    /// The editable global settings. A key is a contour.yml key path -- top-level ("reflow_on_resize")
    /// or dotted into a section ("gutter.line_numbers") -- and needs a loader line in
    /// mergeGuiManagedSideFiles (the section's, for a dotted key), or its override saves and then does
    /// nothing. "SettingsController: every global field's override survives save and reload" drives
    /// every row through the real store and reload, so a row without its loader line fails there.
```

- [ ] **Step 4: Run the tests to verify they pass**

Run:
```bash
clang-format -i src/contour/config/Config.cpp src/contour/window/SettingsController.cpp src/contour/window/SettingsController_test.cpp
cmake --build --preset clangcl-debug --target contour_gui_test
out/build/clangcl-debug/bin/contour_gui_test.exe "[settings]"
```
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/contour/config/Config.cpp src/contour/window/SettingsController.cpp src/contour/window/SettingsController_test.cpp
git commit -F - <<'EOF'
settings: apply the grapheme_clustering and text_scaling_method overrides

Both rows wrote their override to settings.yml, but the load-time merge had
no loader line for either, so the change never took effect and the page
went on showing the old value. A test now drives every global row through
the real store and reload, so a row cannot ship without its loader line.

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

---

### Task 10.3: Global rows for the gutter, command blocks and folding

**Files:**
- Modify: `src/contour/window/SettingsController.cpp` (includes `:9-19`; `GlobalFieldDescriptor` `:914-926`; helpers after `boolToYaml` `:928-931`; rows at the end of `globalFieldDescriptors()` `:1076`; `setGlobalField` `:1539-1555`)
- Modify: `src/contour/window/SettingsController.hpp` (`globalFields` property doc, `:57-60`)
- Test: `src/contour/window/SettingsController_test.cpp`

(`Config.hpp`/`Config.cpp` are untouched: phase 4's `GutterConfig` loader already validates `timestamp_format` with `vtbackend::measureTimestampFormat()`, and the page calls the same function — one check, no wrapper.)

**Interfaces:**
- Consumes: C9 `GutterConfig { bool exitStatus; bool userMarks; vtbackend::LineNumberMode lineNumbers; int lineNumberWidth; bool timestamps; std::string timestampFormat; }`, `CommandBlocksConfig { size_t maxRecords; std::string pager; }`, `FoldingConfig` (`Config.hpp:343-361`); `config::MaxCommandBlockRecords` (phase 1, Task 1.13, the loader's upper clamp of `max_records`); `vtbackend::MinLineNumberWidth` / `vtbackend::MaxLineNumberWidth` (phase 4, Task 4.6); `config::configEnumValues<vtbackend::LineNumberMode>()` (spec §11.1 requires phase 4 to give the enum a `ConfigEnum` table); `config::configEnumValues<vtbackend::FoldJumpBehavior>()` (`ConfigEnum.hpp:145`); `configEnumTokens<Enum>()`, `toQString()`, `boolToYaml()` (`SettingsController.cpp:69-82,928`); phase 4 (Task 4.5, `vtbackend/screen/Gutter.hpp`) `enum class TimestampFormatError : uint8_t { Empty = 0, Malformed, TooWide };`, `[[nodiscard]] std::expected<int, TimestampFormatError> vtbackend::measureTimestampFormat(std::string_view format);` and `[[nodiscard]] std::string_view vtbackend::timestampFormatErrorText(TimestampFormatError error) noexcept;` — the check the `GutterConfig` loader already uses.
- Produces: internal to `SettingsController.cpp`: `using GlobalFieldValidator = std::function<std::expected<void, std::string>(QVariant const&)>;`, `GlobalFieldDescriptor::validate`, and the builders `globalBoolField`, `globalIntField`, `globalStringField`, `globalEnumFieldFromTable<Enum>`. Global keys: `gutter.exit_status`, `gutter.user_marks`, `gutter.line_numbers`, `gutter.line_number_width`, `gutter.timestamps`, `gutter.timestamp_format`, `command_blocks.max_records`, `command_blocks.pager`, `folding.enabled`, `folding.show_markers`, `folding.auto_collapse_on_new_command`, `folding.on_jump_into_fold`.

- [ ] **Step 1: Confirm the consumed token table and the gutter's timestamp check**

Run:
```bash
grep -rn "configEnumValues<vtbackend::LineNumberMode>\|ConfigEnumInfo<vtbackend::LineNumberMode>" src/contour/config
grep -n "measureTimestampFormat\|timestampFormatErrorText" src/vtbackend/screen/Gutter.hpp src/contour/config/Config.cpp
```
Expected: the first finds the `LineNumberMode` specialization (if not, stop and report: phase 4 did not meet spec §11.1, and a second token list here would be exactly the drift `ConfigEnum.hpp` exists to prevent). The second finds both declarations in `Gutter.hpp` and their use in the `GutterConfig` loader (phase 4, Tasks 4.5 and 4.11) — the one timestamp-format check the page will call too. If either is missing, stop and report rather than write a second check.

- [ ] **Step 2: Write the failing tests**

In `src/contour/window/SettingsController_test.cpp` add `#include <vtbackend/screen/Gutter.hpp>`, `#include <vtbackend/shell/Folding.hpp>` and `#include <yaml-cpp/yaml.h>` to the includes, and inside the anonymous namespace add:
```cpp
/// The configuration tokens of every value of configuration enum @p Enum, as the page shows them.
template <typename Enum>
[[nodiscard]] QStringList tokensOf()
{
    auto tokens = QStringList {};
    for (auto const& info: config::configEnumValues<Enum>())
        tokens.push_back(QString::fromUtf8(info.token.data(), static_cast<qsizetype>(info.token.size())));
    return tokens;
}
```
Then, before `TEST_CASE("SettingsController: exposes the configured keybindings read-only"`:
```cpp
TEST_CASE("SettingsController: a dotted global field overrides one leaf of its section", "[settings]")
{
    auto fx = Fixture(R"(
default_profile: main
gutter:
    timestamps: true
    timestamp_format: "%H:%M"
folding:
    show_markers: false
profiles:
    main:
        show_title_bar: true
)");
    auto const configDir = std::filesystem::path(fx.dir.path().toStdString());

    REQUIRE(fx.controller->setGlobalField("gutter.line_numbers", "relative"));
    REQUIRE(fx.controller->setGlobalField("folding.enabled", false));

    // The overrides took ...
    CHECK(fx.cfg.gutter.value().lineNumbers == vtbackend::LineNumberMode::Relative);
    CHECK(fx.cfg.folding.value().enabled == false);
    // ... and nothing else in either section moved.
    CHECK(fx.cfg.gutter.value().timestamps == true);
    CHECK(fx.cfg.gutter.value().timestampFormat == "%H:%M");
    CHECK(fx.cfg.folding.value().showMarkers == false);

    // settings.yml reads like contour.yml: a section holding the one leaf, never a literal dotted key.
    auto const written = YAML::LoadFile((configDir / "settings.yml").string());
    REQUIRE(written["gutter"].IsMap());
    CHECK(written["gutter"]["line_numbers"].as<std::string>() == "relative");
    CHECK(written["gutter"].size() == std::size_t { 1 });
    CHECK_FALSE(written["gutter.line_numbers"].IsDefined());

    // Only the overridden row says so, and Reset brings back what contour.yml says (here: the default).
    CHECK(rowWithKey(fx.controller->globalFields(), "gutter.line_numbers").value("overridden").toBool());
    CHECK_FALSE(rowWithKey(fx.controller->globalFields(), "gutter.timestamps").value("overridden").toBool());
    REQUIRE(fx.controller->resetGlobalField("gutter.line_numbers"));
    CHECK(fx.cfg.gutter.value().lineNumbers == vtbackend::LineNumberMode::Off);
    CHECK(fx.cfg.gutter.value().timestamps == true);
}

TEST_CASE("SettingsController: the command-block enum rows offer the loader's own tokens", "[settings]")
{
    auto fx = Fixture(BasicConfig);
    auto const rows = fx.controller->globalFields();

    // Spelled as the spec spells them, and taken from the table the loader parses with.
    CHECK(tokensOf<vtbackend::LineNumberMode>() == QStringList { "off", "absolute", "relative", "hybrid" });
    CHECK(rowWithKey(rows, "gutter.line_numbers").value("options").toStringList()
          == tokensOf<vtbackend::LineNumberMode>());
    CHECK(rowWithKey(rows, "folding.on_jump_into_fold").value("options").toStringList()
          == tokensOf<vtbackend::FoldJumpBehavior>());

    // A token the loader would reject is refused rather than written.
    CHECK_FALSE(fx.controller->setGlobalField("gutter.line_numbers", "sideways"));
    CHECK_FALSE(rowWithKey(fx.controller->globalFields(), "gutter.line_numbers").value("overridden").toBool());
}

TEST_CASE("SettingsController: an invalid timestamp format is reported and not saved", "[settings]")
{
    auto fx = Fixture(BasicConfig);
    auto errors = QStringList {};
    QObject::connect(fx.controller.get(), &SettingsController::errorOccurred, [&errors](QString const& message) {
        errors.push_back(message);
    });

    CHECK_FALSE(fx.controller->setGlobalField("gutter.timestamp_format", "%K"));
    CHECK_FALSE(fx.controller->setGlobalField("gutter.timestamp_format", "{%H}"));
    CHECK_FALSE(fx.controller->setGlobalField("gutter.timestamp_format", ""));
    REQUIRE(errors.size() == qsizetype { 3 });
    for (auto const& message: errors)
        CHECK(message.contains("timestamp format"));
    CHECK(fx.cfg.gutter.value().timestampFormat == "%H:%M:%S");
    CHECK_FALSE(
        rowWithKey(fx.controller->globalFields(), "gutter.timestamp_format").value("overridden").toBool());

    REQUIRE(fx.controller->setGlobalField("gutter.timestamp_format", "%H:%M"));
    CHECK(fx.cfg.gutter.value().timestampFormat == "%H:%M");
}

TEST_CASE("SettingsController: the numeric command-block rows are range-checked", "[settings]")
{
    auto fx = Fixture(BasicConfig);

    CHECK_FALSE(fx.controller->setGlobalField("gutter.line_number_width", 2));
    CHECK_FALSE(fx.controller->setGlobalField("gutter.line_number_width", 11));
    REQUIRE(fx.controller->setGlobalField("gutter.line_number_width", 10));
    CHECK(fx.cfg.gutter.value().lineNumberWidth == 10);

    CHECK_FALSE(fx.controller->setGlobalField("command_blocks.max_records", 0));
    // The loader clamps above config::MaxCommandBlockRecords; the page refuses instead of saving a value
    // that would silently load as something else.
    CHECK_FALSE(fx.controller->setGlobalField("command_blocks.max_records", 1'000'001));
    REQUIRE(fx.controller->setGlobalField("command_blocks.max_records", 1));
    CHECK(fx.cfg.commandBlocks.value().maxRecords == std::size_t { 1 });
}
```

- [ ] **Step 3: Run the tests to verify they fail**

Run:
```bash
cmake --build --preset clangcl-debug --target contour_gui_test
out/build/clangcl-debug/bin/contour_gui_test.exe "[settings]"
```
Expected: builds; the four new cases FAIL — no `gutter.*`, `command_blocks.*` or `folding.*` row exists yet, so `setGlobalField("gutter.line_numbers", …)` returns false and `rowWithKey(…)` finds nothing.

- [ ] **Step 4: Leave the loader as phase 4 wrote it**

The `GutterConfig` loader keeps phase 4's `timestamp_format` handling (`vtbackend::measureTimestampFormat()`; on failure it logs `vtbackend::timestampFormatErrorText()` and keeps the previous value). The page below calls the same two functions, so the loader and the page cannot disagree about which formats are valid.

- [ ] **Step 5: Give global rows a validator and the four builders**

In `src/contour/window/SettingsController.cpp`, add `#include <vtbackend/screen/Gutter.hpp>` after the `StatusLineBuilder.hpp` include, and `#include <chrono>`, `#include <expected>`, `#include <functional>`, `#include <optional>` to the standard includes (sorted).

Replace the `GlobalFieldDescriptor` block (`:914-926`):

Before:
```cpp
    /// A data-driven descriptor for one editable global (application-scope) setting. `toYaml` turns the
    /// edited value into the YAML scalar written to settings.yml; the load-time merge re-applies it
    /// through the typed per-key loader, so this side needs no parsing.
    struct GlobalFieldDescriptor
    {
        QString key;
        QString label;
        QString help;
        QString type;
        std::function<QVariant(config::Config const&)> get;
        std::function<std::string(QVariant const&)> toYaml;
        QStringList options {}; //!< For "enum": the allowed values (the combo model + accepted set).
    };
```
After:
```cpp
    /// Checks an edited global value before it is written, and says why when it refuses it.
    using GlobalFieldValidator = std::function<std::expected<void, std::string>(QVariant const&)>;

    /// A data-driven descriptor for one editable global (application-scope) setting. `toYaml` turns the
    /// edited value into the YAML scalar written to settings.yml; the load-time merge re-applies it
    /// through the typed per-key loader, so this side needs no parsing.
    struct GlobalFieldDescriptor
    {
        QString key; //!< contour.yml key path: "reflow_on_resize", or "gutter.line_numbers" for a leaf.
        QString label;
        QString help;
        QString type;
        std::function<QVariant(config::Config const&)> get;
        std::function<std::string(QVariant const&)> toYaml;
        QStringList options {}; //!< For "enum": the allowed values (the combo model + accepted set).
        GlobalFieldValidator validate {}; //!< Refuses a value the loader would reject; empty accepts all.
    };
```
After `boolToYaml` (`:928-931`), add:
```cpp
    /// Builds a bool global-field descriptor.
    /// @param key   The contour.yml key path (see GlobalFieldDescriptor::key).
    /// @param label Human-readable label.
    /// @param help  One-line help text.
    /// @param get   Reads the effective value from the configuration.
    /// @return The descriptor.
    GlobalFieldDescriptor globalBoolField(QString key,
                                          QString label,
                                          QString help,
                                          std::function<bool(config::Config const&)> get)
    {
        return { std::move(key),
                 std::move(label),
                 std::move(help),
                 "bool",
                 [get = std::move(get)](config::Config const& c) { return QVariant(get(c)); },
                 [](QVariant const& v) { return boolToYaml(v).toStdString(); } };
    }

    /// Builds an integer global-field descriptor that refuses values outside [@p lowest, @p highest], so
    /// the page cannot write a value the loader would clamp or reject.
    /// @param lowest  The smallest accepted value.
    /// @param highest The largest accepted value; none means unbounded above.
    /// @return The descriptor.
    GlobalFieldDescriptor globalIntField(QString key,
                                         QString label,
                                         QString help,
                                         std::function<int(config::Config const&)> get,
                                         int lowest,
                                         std::optional<int> highest)
    {
        return { std::move(key),
                 std::move(label),
                 std::move(help),
                 "int",
                 [get = std::move(get)](config::Config const& c) { return QVariant(get(c)); },
                 [](QVariant const& v) { return std::to_string(v.toInt()); },
                 {},
                 [lowest, highest](QVariant const& v) -> std::expected<void, std::string> {
                     auto isNumber = false; // Qt's out-parameter
                     auto const number = v.toInt(&isNumber);
                     if (isNumber && number >= lowest && (!highest || number <= *highest))
                         return {};
                     return std::unexpected(highest
                                                ? std::format("Enter a whole number from {} to {}.", lowest, *highest)
                                                : std::format("Enter a whole number of at least {}.", lowest));
                 } };
    }

    /// Builds a string global-field descriptor, optionally checked by @p validate before it is written.
    /// @return The descriptor.
    GlobalFieldDescriptor globalStringField(QString key,
                                            QString label,
                                            QString help,
                                            std::function<std::string(config::Config const&)> get,
                                            GlobalFieldValidator validate = {})
    {
        return { std::move(key),
                 std::move(label),
                 std::move(help),
                 "string",
                 [get = std::move(get)](config::Config const& c) {
                     return QVariant(QString::fromStdString(get(c)));
                 },
                 [](QVariant const& v) { return v.toString().toStdString(); },
                 {},
                 std::move(validate) };
    }

    /// Builds an enum global-field descriptor whose options, displayed value and accepted set all come
    /// from the enum's ConfigEnum table -- the one the loader parses with -- so the page can neither
    /// offer nor write a token the loader rejects.
    /// @return The descriptor.
    template <typename Enum>
    GlobalFieldDescriptor globalEnumFieldFromTable(QString key,
                                                   QString label,
                                                   QString help,
                                                   std::function<Enum(config::Config const&)> get)
    {
        return { std::move(key),
                 std::move(label),
                 std::move(help),
                 "enum",
                 [get = std::move(get)](config::Config const& c) {
                     return QVariant(toQString(config::configEnumToken(get(c))));
                 },
                 [](QVariant const& v) { return v.toString().toStdString(); },
                 configEnumTokens<Enum>(),
                 [](QVariant const& v) -> std::expected<void, std::string> {
                     auto const token = v.toString().toStdString();
                     if (config::configEnumFromToken<Enum>(token))
                         return {};
                     return std::unexpected(std::format("'{}' is not one of the offered values.", token));
                 } };
    }

    /// The gutter's own timestamp-format check (vtbackend::measureTimestampFormat -- the one the
    /// GutterConfig loader uses), phrased for the page; setGlobalField() reports a refusal through
    /// errorOccurred with vtbackend::timestampFormatErrorText()'s reason.
    /// @param value The format the user typed.
    /// @return Nothing when valid, or the message the page shows.
    [[nodiscard]] std::expected<void, std::string> checkTimestampFormat(QVariant const& value)
    {
        return vtbackend::measureTimestampFormat(value.toString().toStdString())
            .transform([](int /*columns*/) {})
            .transform_error([](vtbackend::TimestampFormatError error) {
                return std::format("Gutter timestamp format: {}.", vtbackend::timestampFormatErrorText(error));
            });
    }
```

- [ ] **Step 6: Add the twelve rows**

At the end of the `globalFieldDescriptors()` vector, after the `text_scaling_method` row's closing `{ "stretch", "rerasterize" } },` (`:1076`), add:
```cpp
            // {{{ Command blocks. A dotted key is a path to one leaf of a contour.yml section;
            // settings.yml stores it nested and the merge applies that leaf alone (see
            // mergeGuiManagedSideFiles), so a row here never resets its section's other keys.
            globalBoolField("gutter.exit_status",
                            "Gutter: exit status",
                            "Colour a command's marks in the gutter by how it ended, and mark commands "
                            "still running or finished without output.",
                            [](config::Config const& c) { return c.gutter.value().exitStatus; }),
            globalBoolField("gutter.user_marks",
                            "Gutter: user marks",
                            "Show the lines you marked with Vi mode's mm in the gutter.",
                            [](config::Config const& c) { return c.gutter.value().userMarks; }),
            globalEnumFieldFromTable<vtbackend::LineNumberMode>(
                "gutter.line_numbers",
                "Gutter: line numbers",
                "off, absolute (counted from the session's first line), relative (distance from the "
                "cursor line) or hybrid (the cursor line absolute, the others relative).",
                [](config::Config const& c) { return c.gutter.value().lineNumbers; }),
            globalIntField("gutter.line_number_width",
                           "Gutter: line number width",
                           "Columns reserved for line numbers, 3 to 10. Changing it resizes the page.",
                           [](config::Config const& c) { return c.gutter.value().lineNumberWidth; },
                           vtbackend::MinLineNumberWidth,
                           vtbackend::MaxLineNumberWidth),
            globalBoolField("gutter.timestamps",
                            "Gutter: timestamps",
                            "Show beside each line the time it was first written.",
                            [](config::Config const& c) { return c.gutter.value().timestamps; }),
            globalStringField("gutter.timestamp_format",
                              "Gutter: timestamp format",
                              "A std::format chrono format in local time, such as %H:%M:%S.",
                              [](config::Config const& c) { return c.gutter.value().timestampFormat; },
                              checkTimestampFormat),
            globalIntField("command_blocks.max_records",
                           "Command blocks: records kept",
                           "How many commands are remembered (command line, exit status, times, "
                           "directory), including ones whose output has left the history.",
                           [](config::Config const& c) {
                               return static_cast<int>(c.commandBlocks.value().maxRecords);
                           },
                           1,
                           static_cast<int>(config::MaxCommandBlockRecords)),
            globalStringField("command_blocks.pager",
                              "Command blocks: pager",
                              "The program a command's output is opened in when an OpenCommandOutput "
                              "binding names none.",
                              [](config::Config const& c) { return c.commandBlocks.value().pager; }),
            globalBoolField("folding.enabled",
                            "Folding: enabled",
                            "Whether a finished command's output can be collapsed to its prompt line.",
                            [](config::Config const& c) { return c.folding.value().enabled; }),
            globalBoolField("folding.show_markers",
                            "Folding: fold markers",
                            "Draw the fold controls in the gutter's block column.",
                            [](config::Config const& c) { return c.folding.value().showMarkers; }),
            globalBoolField("folding.auto_collapse_on_new_command",
                            "Folding: collapse on next prompt",
                            "Collapse a command's output as soon as the next prompt appears.",
                            [](config::Config const& c) { return c.folding.value().autoCollapseOnNewCommand; }),
            globalEnumFieldFromTable<vtbackend::FoldJumpBehavior>(
                "folding.on_jump_into_fold",
                "Folding: jump into a fold",
                "What a Vi-mode jump to a line inside a collapsed block does: expand it, or skip to "
                "its prompt line.",
                [](config::Config const& c) { return c.folding.value().onJumpIntoFold; }),
            // }}}
```

- [ ] **Step 7: Refuse an invalid value before writing it**

In `SettingsController::setGlobalField` (`:1539-1555`), insert the validation as the first statement inside the matching descriptor's branch:

Before:
```cpp
        if (descriptor.key == key)
        {
            auto settings = _config().guiManagedSettings;
            settings.globalOverrides[key.toStdString()] = descriptor.toYaml(value);
```
After:
```cpp
        if (descriptor.key == key)
        {
            if (auto const valid = descriptor.validate ? descriptor.validate(value)
                                                       : std::expected<void, std::string> {};
                !valid)
            {
                // Re-announce the model so the editor shows the stored value again, not the text that
                // was just refused.
                emit changed();
                return fail(valid.error());
            }
            auto settings = _config().guiManagedSettings;
            settings.globalOverrides[key.toStdString()] = descriptor.toYaml(value);
```
(The rest of the function is unchanged.)

In `SettingsController.hpp`, replace the `globalFields` property doc (`:57-59`):

Before:
```cpp
    /// The editable global (application-scope) settings: one QVariantMap per field
    /// { key, label, help, type, value, options, overridden }. Edits are written to settings.yml as
    /// overrides on contour.yml — the same side-file discipline the rest of the page uses.
```
After:
```cpp
    /// The editable global (application-scope) settings: one QVariantMap per field
    /// { key, label, help, type, value, options, overridden }. Edits are written to settings.yml as
    /// overrides on contour.yml — the same side-file discipline the rest of the page uses. A dotted
    /// key ("gutter.line_numbers") is a path to one leaf of a contour.yml section, and overrides only
    /// that leaf. A value the loader would reject is refused with errorOccurred and not written.
```

- [ ] **Step 8: Run the tests to verify they pass**

Run:
```bash
clang-format -i src/contour/window/SettingsController.hpp src/contour/window/SettingsController.cpp src/contour/window/SettingsController_test.cpp
cmake --build --preset clangcl-debug --target contour_gui_test
out/build/clangcl-debug/bin/contour_gui_test.exe "[config]"
out/build/clangcl-debug/bin/contour_gui_test.exe "[settings]"
```
Expected: PASS — including phase 4's existing `timestamp_format` loader test (the loader is untouched: invalid → logged, previous kept) and Task 10.2's every-row test, which now also drives the twelve new rows (`"%H:%M:%S"` → `"x"` is a valid literal format; `6` → `7`, `1000` → `1001` are in range).

- [ ] **Step 9: Hand-audit for clang-tidy and commit**

Check the added lines for `misc-const-correctness` (every never-mutated local `const`), `misc-use-internal-linkage` (the new helpers, `checkTimestampFormat` included, sit inside the file's anonymous namespace, `SettingsController.cpp:28-1140`), and that no identifier is a Windows macro name. Then:
```bash
git add src/contour/window/SettingsController.hpp src/contour/window/SettingsController.cpp src/contour/window/SettingsController_test.cpp
git commit -F - <<'EOF'
settings: edit the gutter, command-block and folding keys on the page

Twelve global rows with dotted keys, each overriding one leaf of its
contour.yml section. Enum rows take their options from the loader's token
tables, the two numeric rows are range-checked, and the timestamp format
goes through vtbackend::measureTimestampFormat -- the check the gutter's
loader already uses -- so the page reports a value the loader would
reject instead of writing it.

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

---

### Task 10.4: The "Command blocks" profile group

**Files:**
- Modify: `src/contour/window/SettingsController.cpp` (includes; helpers after `boolField` `:57-67`; a new group in `profileFieldGroups()` between "History & scrollbar" (ends `:514`) and "Mouse & permissions" (`:515`))
- Test: `src/contour/window/SettingsController_test.cpp` (profile-field section, before its closing `// }}}` at `:661`), `src/contour/test/SettingsPageQml_test.cpp` (append)
- Modify (only if Step 6's contingency applies): `src/contour/config/Config.hpp` / `src/contour/config/Config.cpp` (phase 8's `scrollbar.marks` writer)

**Interfaces:**
- Consumes: C9/C3 `TerminalProfile::stickyHeader` (`ConfigEntry<StickyHeaderConfig, …>`, `StickyHeaderConfig { vtbackend::StickyHeaderMode mode; bool showEvicted; }`, phase 7); C7 `TerminalProfile::notifyOnCommandFinish` (`ConfigEntry<FinishNotificationConfig, …>`, phase 6) with `NotifyWhen`, `NotifyOutcome`, `NotifyAction`, `NotifyClearOn`, `std::chrono::seconds minDuration`; C4 `ScrollBarConfig::marks` (`vtbackend::ScrollbarMarkSources`, phase 8); `config::configEnumValues<…>()` for `vtbackend::StickyHeaderMode` and the four `Notify*` enums (spec §11.1); `core::Flags::test/enable/disable` (`vendor/core-cpp/src/core/Flags.hpp:49-63`); `enumFieldFromTable<Enum>()` (`SettingsController.cpp:137-153`).
- Produces: profile row keys, in order: `scrollbar_marks_failures`, `scrollbar_marks_commands`, `scrollbar_marks_user_marks`, `sticky_header_mode`, `sticky_header_show_evicted`, `notify_on_command_finish_when`, `notify_on_command_finish_min_duration`, `notify_on_command_finish_outcome`, `notify_on_command_finish_action`, `notify_on_command_finish_clear_on` (group title `Command blocks`, glyph `❯`). Internal helpers `memberBoolField(...)` and `scrollbarMarkField(...)`.

- [ ] **Step 1: Confirm the consumed names and tables**

Run:
```bash
grep -n "stickyHeader\b\|notifyOnCommandFinish\b" src/contour/config/Config.hpp
grep -n "ScrollbarMarkSources marks\|marks {" src/contour/config/Config.hpp
grep -rn "ConfigEnumInfo<vtbackend::StickyHeaderMode>\|ConfigEnumInfo<NotifyWhen>\|ConfigEnumInfo<NotifyOutcome>\|ConfigEnumInfo<NotifyAction>\|ConfigEnumInfo<NotifyClearOn>" src/contour/config
```
Expected: the two profile members, the `marks` member of `ScrollBarConfig`, and five token tables. A member under another name: use the real name throughout this task. A missing member or table: stop and report (C9 / spec §11.1 not met).

- [ ] **Step 2: Write the failing tests**

In `SettingsController_test.cpp` add `#include <vtbackend/shell/ScrollbarMarks.hpp>`, `#include <vtbackend/shell/StickyHeader.hpp>` and `#include <chrono>`. Before the `// }}}` that closes `// {{{ Profile field descriptors`:
```cpp
TEST_CASE("SettingsController: the command-block profile fields round-trip through a saved profile",
          "[settings]")
{
    auto fx = Fixture(BasicConfig);
    fx.controller->newProfile("main");

    fx.controller->setProfileField("scrollbar_marks_commands", false);
    fx.controller->setProfileField("sticky_header_mode", "always");
    fx.controller->setProfileField("sticky_header_show_evicted", false);
    fx.controller->setProfileField("notify_on_command_finish_when", "hidden");
    fx.controller->setProfileField("notify_on_command_finish_min_duration", 42);
    fx.controller->setProfileField("notify_on_command_finish_outcome", "failure");
    fx.controller->setProfileField("notify_on_command_finish_action", "notify_bell");
    fx.controller->setProfileField("notify_on_command_finish_clear_on", "next");
    REQUIRE(fx.controller->saveProfileAs("blocks"));

    auto const* blocks = fx.cfg.findProfile("blocks");
    REQUIRE(blocks != nullptr);
    using vtbackend::ScrollbarMarkSource;
    auto const marks = blocks->scrollbar.value().marks;
    CHECK(marks.test(ScrollbarMarkSource::Failures));
    CHECK_FALSE(marks.test(ScrollbarMarkSource::Commands));
    CHECK(marks.test(ScrollbarMarkSource::UserMarks));
    CHECK(blocks->stickyHeader.value().mode == vtbackend::StickyHeaderMode::Always);
    CHECK(blocks->stickyHeader.value().showEvicted == false);
    auto const& notify = blocks->notifyOnCommandFinish.value();
    CHECK(notify.when == config::NotifyWhen::Hidden);
    CHECK(notify.minDuration == std::chrono::seconds(42));
    CHECK(notify.outcome == config::NotifyOutcome::Failure);
    CHECK(notify.action == config::NotifyAction::NotifyBell);
    CHECK(notify.clearOn == config::NotifyClearOn::Next);

    // The page reads back what was saved (saveProfileAs re-opens the reloaded profile).
    auto const fields = fx.controller->profileFields();
    CHECK(rowWithKey(fields, "scrollbar_marks_commands").value("value").toBool() == false);
    CHECK(rowWithKey(fields, "sticky_header_mode").value("value").toString() == "always");
    CHECK(rowWithKey(fields, "notify_on_command_finish_min_duration").value("value").toInt() == 42);

    SECTION("all three scrollbar sources off is an empty list, not the default")
    {
        fx.controller->setProfileField("scrollbar_marks_failures", false);
        fx.controller->setProfileField("scrollbar_marks_user_marks", false);
        REQUIRE(fx.controller->saveProfile());
        auto const* saved = fx.cfg.findProfile("blocks");
        REQUIRE(saved != nullptr);
        CHECK(saved->scrollbar.value().marks.none());
    }

    SECTION("a negative minimum duration is stored as zero")
    {
        fx.controller->setProfileField("notify_on_command_finish_min_duration", -5);
        CHECK(rowWithKey(fx.controller->profileFields(), "notify_on_command_finish_min_duration")
                  .value("value")
                  .toInt()
              == 0);
    }
}

TEST_CASE("SettingsController: the command-block profile fields form their own group", "[settings]")
{
    auto fx = Fixture(BasicConfig);
    fx.controller->newProfile("main");

    auto keys = QStringList {};
    for (auto const& raw: fx.controller->profileFields())
        if (raw.toMap().value("group").toString() == "Command blocks")
            keys.push_back(raw.toMap().value("key").toString());
    CHECK(keys
          == QStringList { "scrollbar_marks_failures",
                           "scrollbar_marks_commands",
                           "scrollbar_marks_user_marks",
                           "sticky_header_mode",
                           "sticky_header_show_evicted",
                           "notify_on_command_finish_when",
                           "notify_on_command_finish_min_duration",
                           "notify_on_command_finish_outcome",
                           "notify_on_command_finish_action",
                           "notify_on_command_finish_clear_on" });

    // The enum rows offer exactly the spec's spellings -- which are the loader's tables' tokens, since
    // the rows read their options from those tables.
    auto const fields = fx.controller->profileFields();
    auto const optionsOf = [&fields](QString const& key) {
        return rowWithKey(fields, key).value("options").toStringList();
    };
    CHECK(optionsOf("sticky_header_mode") == QStringList { "never", "scrolled", "always" });
    CHECK(optionsOf("notify_on_command_finish_when") == QStringList { "never", "unfocused", "hidden", "always" });
    CHECK(optionsOf("notify_on_command_finish_outcome") == QStringList { "any", "failure" });
    CHECK(optionsOf("notify_on_command_finish_action") == QStringList { "notify", "bell", "notify_bell" });
    CHECK(optionsOf("notify_on_command_finish_clear_on") == QStringList { "focus", "next", "never" });
}
```
Append to `src/contour/test/SettingsPageQml_test.cpp`:
```cpp
TEST_CASE("SettingsPage renders the command-block settings from the descriptors", "[contour][gui][qml][settings]")
{
    // No QML knows about these rows: the profile pane groups whatever `group` a row carries, and the
    // global pane lists whatever globalFields returns. This proves both hold for the new rows.
    contour::test::QmlMessageCapture const warnings;

    auto fx = PageFixture(OneProfile);
    auto* item = fx.item();
    REQUIRE(item != nullptr);
    clickButton(item->findChild<QObject*>("newProfileButton"));
    fx.settle();

    auto groupKeys = QStringList {};
    for (auto const& raw: fx.page->property("profileGroups").toList())
        if (raw.toMap().value("title").toString() == "Command blocks")
            for (auto const& field: raw.toMap().value("fields").toList())
                groupKeys.push_back(field.toMap().value("key").toString());
    CHECK(groupKeys.size() == qsizetype { 10 });
    CHECK(groupKeys.contains("sticky_header_mode"));

    // A user who knows the YAML path can search for it on the global pane.
    fx.page->setProperty("filterText", "gutter.");
    fx.settle();
    auto globalKeys = QStringList {};
    for (auto const& raw: fx.page->property("globalFieldsFiltered").toList())
        globalKeys.push_back(raw.toMap().value("key").toString());
    for (auto const* key: { "gutter.exit_status",
                            "gutter.user_marks",
                            "gutter.line_numbers",
                            "gutter.line_number_width",
                            "gutter.timestamps",
                            "gutter.timestamp_format" })
        CHECK(globalKeys.contains(QString::fromLatin1(key)));

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}
```

- [ ] **Step 3: Run the tests to verify they fail**

Run:
```bash
cmake --build --preset clangcl-debug --target contour_gui_test
out/build/clangcl-debug/bin/contour_gui_test.exe "SettingsController: the command-block profile fields*"
out/build/clangcl-debug/bin/contour_gui_test.exe "SettingsPage renders the command-block settings from the descriptors"
```
Expected: FAIL — `setProfileField` ignores the unknown keys (the saved profile keeps its defaults), the group is empty, and the QML test's `groupKeys.size() == 10` fails. The QML test's global half already PASSES (Task 10.3's rows render with no QML change).

- [ ] **Step 4: Add the two builders**

In `SettingsController.cpp`, add `#include <vtbackend/shell/ScrollbarMarks.hpp>` and `#include <vtbackend/shell/StickyHeader.hpp>` next to the `Gutter.hpp` include. After `boolField` (`:57-67`), add:
```cpp
    /// Builds a bool profile-field descriptor for one bool member of a struct-valued ConfigEntry, such
    /// as `sticky_header.show_evicted`.
    /// @param entry  Returns the ConfigEntry holding the struct, for a const and a non-const profile alike.
    /// @param member The bool member of that struct this row edits.
    /// @return The descriptor.
    template <typename EntryAccessor, typename Struct>
    ProfileFieldDescriptor memberBoolField(
        QString key, QString label, QString help, EntryAccessor entry, bool Struct::* member)
    {
        return { std::move(key),
                 std::move(label),
                 std::move(help),
                 "bool",
                 [entry, member](TerminalProfile const& p) { return QVariant(entry(p).value().*member); },
                 [entry, member](TerminalProfile& p, QVariant const& v) {
                     entry(p).value().*member = v.toBool();
                 } };
    }

    /// Builds one `scrollbar.marks` source as a checkbox. The YAML list holds the sources that are on,
    /// so the row is "is this source in the list" -- three checkboxes rather than a list editor, because
    /// the list is a set of three known values, not free text.
    /// @param source The source this row switches.
    /// @return The descriptor.
    ProfileFieldDescriptor scrollbarMarkField(QString key,
                                              QString label,
                                              QString help,
                                              vtbackend::ScrollbarMarkSource source)
    {
        return { std::move(key),
                 std::move(label),
                 std::move(help),
                 "bool",
                 [source](TerminalProfile const& p) {
                     return QVariant(p.scrollbar.value().marks.test(source));
                 },
                 [source](TerminalProfile& p, QVariant const& v) {
                     auto& marks = p.scrollbar.value().marks;
                     if (v.toBool())
                         marks.enable(source);
                     else
                         marks.disable(source);
                 } };
    }
```

- [ ] **Step 5: Add the group**

In `profileFieldGroups()`, between the "History & scrollbar" group's closing `} },` (`:514`) and `{ "Mouse & permissions",` (`:515`), insert:
```cpp
            { "Command blocks",
              "❯",
              {
                  scrollbarMarkField("scrollbar_marks_failures",
                                     "Scrollbar marks: failed commands",
                                     "Mark every command that failed on the scrollbar. Click a mark to jump to it.",
                                     vtbackend::ScrollbarMarkSource::Failures),
                  scrollbarMarkField("scrollbar_marks_commands",
                                     "Scrollbar marks: every command",
                                     "Mark every command's prompt on the scrollbar.",
                                     vtbackend::ScrollbarMarkSource::Commands),
                  scrollbarMarkField("scrollbar_marks_user_marks",
                                     "Scrollbar marks: user marks",
                                     "Mark the lines you marked with Vi mode's mm on the scrollbar.",
                                     vtbackend::ScrollbarMarkSource::UserMarks),
                  enumFieldFromTable<vtbackend::StickyHeaderMode>(
                      "sticky_header_mode",
                      "Sticky command header",
                      "Pin the command whose output fills the top of the page above it: never, only while "
                      "scrolled back into the history (scrolled), or also while following live output "
                      "(always).",
                      [](TerminalProfile const& p) { return p.stickyHeader.value().mode; },
                      [](TerminalProfile& p, auto v) { p.stickyHeader.value().mode = v; }),
                  memberBoolField("sticky_header_show_evicted",
                                  "Name evicted commands",
                                  "When a command's own line was evicted from the history, name it in the header "
                                  "instead: wherever the header shows, and at the very top of the history even "
                                  "when the sticky header is set to never.",
                                  [](auto& p) -> auto& { return p.stickyHeader; },
                                  &config::StickyHeaderConfig::showEvicted),
                  enumFieldFromTable<config::NotifyWhen>(
                      "notify_on_command_finish_when",
                      "Notify on finish",
                      "When a finished command raises a notification: never, when its pane is not focused "
                      "(unfocused), only when its tab is not shown at all (hidden), or always.",
                      [](TerminalProfile const& p) { return p.notifyOnCommandFinish.value().when; },
                      [](TerminalProfile& p, auto v) { p.notifyOnCommandFinish.value().when = v; }),
                  { "notify_on_command_finish_min_duration",
                    "Notify: minimum duration (s)",
                    "Commands that finish faster than this never notify.",
                    "int",
                    [](TerminalProfile const& p) {
                        return QVariant(static_cast<int>(p.notifyOnCommandFinish.value().minDuration.count()));
                    },
                    [](TerminalProfile& p, QVariant const& v) {
                        p.notifyOnCommandFinish.value().minDuration = std::chrono::seconds { std::max(0, v.toInt()) };
                    } },
                  enumFieldFromTable<config::NotifyOutcome>(
                      "notify_on_command_finish_outcome",
                      "Notify: outcome",
                      "Notify for every command (any), or only for one that failed (failure).",
                      [](TerminalProfile const& p) { return p.notifyOnCommandFinish.value().outcome; },
                      [](TerminalProfile& p, auto v) { p.notifyOnCommandFinish.value().outcome = v; }),
                  enumFieldFromTable<config::NotifyAction>(
                      "notify_on_command_finish_action",
                      "Notify: how",
                      "A desktop notification (notify), the profile's bell (bell), or both (notify_bell).",
                      [](TerminalProfile const& p) { return p.notifyOnCommandFinish.value().action; },
                      [](TerminalProfile& p, auto v) { p.notifyOnCommandFinish.value().action = v; }),
                  enumFieldFromTable<config::NotifyClearOn>(
                      "notify_on_command_finish_clear_on",
                      "Notify: withdraw on",
                      "Withdraw the notification when the pane is focused (focus), when the next command "
                      "there starts (next), or never.",
                      [](TerminalProfile const& p) { return p.notifyOnCommandFinish.value().clearOn; },
                      [](TerminalProfile& p, auto v) { p.notifyOnCommandFinish.value().clearOn = v; }),
              } },
```

- [ ] **Step 6: Run the tests to verify they pass**

Run:
```bash
clang-format -i src/contour/window/SettingsController.cpp src/contour/window/SettingsController_test.cpp src/contour/test/SettingsPageQml_test.cpp
cmake --build --preset clangcl-debug --target contour_gui_test
out/build/clangcl-debug/bin/contour_gui_test.exe "[settings]"
```
Expected: PASS, including the existing `every enum field's value is one of its own options` and `enum fields round-trip through their options`, which now also walk the five new enum rows. If the empty-list SECTION fails, phase 8's `scrollbar.marks` writer emits nothing for an empty set (so the loader falls back to the default): make the writer emit `marks: []` — an empty list is a choice, not an absence. That writer is phase 8's, in `src/contour/config/Config.hpp` / `Config.cpp`: after changing it run `clang-format -i src/contour/config/Config.hpp src/contour/config/Config.cpp`, rebuild and re-run the line above; Step 7 stages both files and its commit body gains the line "The scrollbar.marks writer now emits an empty list as `marks: []`."

- [ ] **Step 7: Commit**

```bash
git add src/contour/window/SettingsController.cpp src/contour/window/SettingsController_test.cpp src/contour/test/SettingsPageQml_test.cpp
git add src/contour/config/Config.hpp src/contour/config/Config.cpp # Step 6's contingency only; a no-op when unchanged
git commit -F - <<'EOF'
settings: add a Command blocks group to the profile page

Scrollbar mark sources as three checkboxes, the sticky header's mode and
evicted-output note, and the five finish-notification keys. Enum options
come from the loader's token tables; the QML renders the group from the
descriptors with no change of its own.

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

---

### Task 10.5: Colour-scheme editor slots for the command-block colours

**Files:**
- Modify: `src/contour/window/SettingsController.cpp` (a derived-slot table before `schemeColorDescriptors()` `:881`; rows appended after the ANSI banks `:908`)
- Modify: `src/contour/qml/ColorSchemeEditor.qml` (header comment `:2-5` only)
- Test: `src/contour/window/SettingsController_test.cpp` (after `TEST_CASE("SettingsController: create, edit and reload a color scheme"`)
- Modify (only if Step 5's contingency applies): `src/contour/config/Config.hpp` / `src/contour/config/Config.cpp` (the colour-scheme writer)

**Interfaces:**
- Consumes: C2 `ColorPalette::blockStatusSuccess`, `blockStatusFailure`, `blockStatusRunning`, `gutterText` (`std::optional<RGBColor>`), `[[nodiscard]] RGBColor ColorPalette::blockStatusColor(CommandBlockOutcome) const noexcept`, `[[nodiscard]] RGBColor ColorPalette::gutterTextColor() const noexcept`, `enum class CommandBlockOutcome : uint8_t { Success = 0, Failure, Running }` (`vtbackend/core/CommandBlockOutcome.hpp`, phase 1; `vtbackend/shell/CommandBlock.hpp` includes it); C3 `ColorPalette::stickyHeaderBackground`, `stickyHeaderSeparator` (`std::optional<RGBColor>`). **Contract addition:** C3 says "with resolvers" without naming them; this phase relies on
  ```cpp
  [[nodiscard]] RGBColor ColorPalette::stickyHeaderBackgroundColor() const noexcept;
  [[nodiscard]] RGBColor ColorPalette::stickyHeaderSeparatorColor() const noexcept;
  ```
  `config::loadColorSchemeFile` (`Config.hpp`).
- Produces: scheme slot keys `block_status_success`, `block_status_failure`, `block_status_running`, `gutter_text`, `sticky_header_background`, `sticky_header_separator`.

- [ ] **Step 1: Confirm the resolvers**

Run: `grep -n "blockStatusColor\|gutterTextColor\|stickyHeader.*Color() const" src/vtbackend/core/ColorPalette.hpp`
Expected: four resolvers. If phase 7 named the sticky-header resolvers differently, use its names in Step 4 and record the actual names in this task's commit body.

- [ ] **Step 2: Write the failing test**

Add `#include <vtbackend/shell/CommandBlock.hpp>` to `SettingsController_test.cpp`; inside the anonymous namespace add:
```cpp
/// "#rrggbb" for @p color, as the scheme editor shows it.
[[nodiscard]] QString hexOf(vtbackend::RGBColor color)
{
    return QString::asprintf("#%02x%02x%02x", color.red, color.green, color.blue);
}
```
After `TEST_CASE("SettingsController: create, edit and reload a color scheme"`:
```cpp
TEST_CASE("SettingsController: the command-block colour slots are editable and stay derived until set",
          "[settings]")
{
    auto fx = Fixture(BasicConfig);
    fx.controller->newColorScheme("");

    // Each slot shows the colour the palette resolves it to while the scheme leaves it unset.
    auto const palette = vtbackend::ColorPalette {};
    auto const colorOf = [&fx](QString const& key) {
        return rowWithKey(fx.controller->schemeColors(), key).value("color").toString();
    };
    CHECK(colorOf("block_status_success") == hexOf(palette.blockStatusColor(vtbackend::CommandBlockOutcome::Success)));
    CHECK(colorOf("block_status_failure") == hexOf(palette.blockStatusColor(vtbackend::CommandBlockOutcome::Failure)));
    CHECK(colorOf("block_status_running") == hexOf(palette.blockStatusColor(vtbackend::CommandBlockOutcome::Running)));
    CHECK(colorOf("gutter_text") == hexOf(palette.gutterTextColor()));
    CHECK(colorOf("sticky_header_background") == hexOf(palette.stickyHeaderBackgroundColor()));
    CHECK(colorOf("sticky_header_separator") == hexOf(palette.stickyHeaderSeparatorColor()));

    fx.controller->setSchemeColor("block_status_failure", "#ff0000");
    fx.controller->setSchemeColor("sticky_header_separator", "#123456");
    REQUIRE(fx.controller->saveColorScheme("blocks"));

    auto const saved = config::loadColorSchemeFile(std::filesystem::path(fx.dir.path().toStdString())
                                                   / "colorschemes" / "blocks.yml");
    REQUIRE(saved.has_value());
    CHECK(saved->blockStatusFailure == vtbackend::RGBColor { 0xff, 0x00, 0x00 });
    CHECK(saved->stickyHeaderSeparator == vtbackend::RGBColor { 0x12, 0x34, 0x56 });

    // The slots nobody edited are still unset, so they keep following the colours they derive from.
    // Seeding the draft with resolved values would have pinned colours the scheme never chose.
    CHECK_FALSE(saved->blockStatusSuccess.has_value());
    CHECK_FALSE(saved->blockStatusRunning.has_value());
    CHECK_FALSE(saved->gutterText.has_value());
    CHECK_FALSE(saved->stickyHeaderBackground.has_value());
}
```

- [ ] **Step 3: Run the test to verify it fails**

Run:
```bash
cmake --build --preset clangcl-debug --target contour_gui_test
out/build/clangcl-debug/bin/contour_gui_test.exe "SettingsController: the command-block colour slots are editable and stay derived until set"
```
Expected: FAIL — `colorOf(...)` is empty for every new key (no such slot), and the saved scheme has no `block_status` failure colour.

- [ ] **Step 4: Add the slot table and its rows**

In `SettingsController.cpp`, add `#include <vtbackend/shell/CommandBlock.hpp>`. Between `struct SchemeColorDescriptor { … };` (`:873-879`) and `schemeColorDescriptors()` (`:881`), add:
```cpp
    /// A colour slot a scheme may leave unset, in which case the palette derives it. The editor row
    /// shows the colour in effect and writes an explicit one only when edited.
    struct DerivedColorSlot
    {
        std::string_view key;                                                //!< QML key.
        std::string_view label;                                              //!< Swatch label.
        std::optional<vtbackend::RGBColor> vtbackend::ColorPalette::* slot; //!< The optional it writes.
        vtbackend::RGBColor (*resolve)(vtbackend::ColorPalette const&);      //!< The colour in effect.
    };

    /// The command-block colour slots, in the order the editor lists them after the ANSI banks. Adding
    /// a derived slot to the scheme editor is adding a row here.
    constexpr auto DerivedColorSlots = std::array {
        DerivedColorSlot { "block_status_success",
                           "Block status: success",
                           &vtbackend::ColorPalette::blockStatusSuccess,
                           [](vtbackend::ColorPalette const& p) {
                               return p.blockStatusColor(vtbackend::CommandBlockOutcome::Success);
                           } },
        DerivedColorSlot { "block_status_failure",
                           "Block status: failure",
                           &vtbackend::ColorPalette::blockStatusFailure,
                           [](vtbackend::ColorPalette const& p) {
                               return p.blockStatusColor(vtbackend::CommandBlockOutcome::Failure);
                           } },
        DerivedColorSlot { "block_status_running",
                           "Block status: running",
                           &vtbackend::ColorPalette::blockStatusRunning,
                           [](vtbackend::ColorPalette const& p) {
                               return p.blockStatusColor(vtbackend::CommandBlockOutcome::Running);
                           } },
        DerivedColorSlot { "gutter_text",
                           "Gutter text",
                           &vtbackend::ColorPalette::gutterText,
                           [](vtbackend::ColorPalette const& p) { return p.gutterTextColor(); } },
        DerivedColorSlot { "sticky_header_background",
                           "Sticky header: background",
                           &vtbackend::ColorPalette::stickyHeaderBackground,
                           [](vtbackend::ColorPalette const& p) { return p.stickyHeaderBackgroundColor(); } },
        DerivedColorSlot { "sticky_header_separator",
                           "Sticky header: separator",
                           &vtbackend::ColorPalette::stickyHeaderSeparator,
                           [](vtbackend::ColorPalette const& p) { return p.stickyHeaderSeparatorColor(); } },
    };
```
In `schemeColorDescriptors()`, replace the doc comment and add the rows after the ANSI loop:

Before:
```cpp
    /// The color slots the scheme editor exposes: the default fg/bg plus the 8 normal and 8 bright
    /// ANSI colors. Generated once from the ANSI names so the two 8-color banks are not hand-listed.
```
After:
```cpp
    /// The color slots the scheme editor exposes: the default fg/bg, the 8 normal and 8 bright ANSI
    /// colors (generated from the ANSI names so the two banks are not hand-listed), then the derived
    /// command-block slots of DerivedColorSlots.
```
Before (end of the generating lambda):
```cpp
                                     [slot](auto const& p) { return p.palette.at(slot); },
                                     [slot](auto& p, auto c) { p.palette.at(slot) = c; } });
                }
            return list;
```
After:
```cpp
                                     [slot](auto const& p) { return p.palette.at(slot); },
                                     [slot](auto& p, auto c) { p.palette.at(slot) = c; } });
                }
            for (auto const& derived: DerivedColorSlots)
                list.push_back({ toQString(derived.key),
                                 toQString(derived.label),
                                 [resolve = derived.resolve](auto const& p) { return resolve(p); },
                                 [slot = derived.slot](auto& p, auto c) { p.*slot = c; } });
            return list;
```
In `src/contour/qml/ColorSchemeEditor.qml`, replace the first comment line pair:

Before:
```qml
// The color-scheme editor: a scrollable grid of swatch cards (default fg/bg + the 8 normal and 8
// bright ANSI colors) bound to SettingsController.schemeColors ({ key, label, color }). Each card
```
After:
```qml
// The color-scheme editor: a scrollable grid of swatch cards (default fg/bg, the 8 normal and 8
// bright ANSI colors, and the command-block slots) bound to SettingsController.schemeColors
// ({ key, label, color }). Each card
```

- [ ] **Step 5: Run the tests to verify they pass**

Run:
```bash
clang-format -i src/contour/window/SettingsController.cpp src/contour/window/SettingsController_test.cpp
cmake --build --preset clangcl-debug --target contour_gui_test
out/build/clangcl-debug/bin/contour_gui_test.exe "[settings]"
```
Expected: PASS. If the "still unset" checks fail, the scheme writer (phase 4 / 7) emits a resolved colour for an unset slot; change it to the `fold_marker` rule (`Config.cpp:3916-3933`: emit only what is set, the commented example otherwise). After that change run `clang-format -i src/contour/config/Config.hpp src/contour/config/Config.cpp`, rebuild and re-run the line above; Step 6 stages both files and its commit body gains the line "The scheme writer now emits an unset derived slot only as a commented example."

- [ ] **Step 6: Commit**

```bash
git add src/contour/window/SettingsController.cpp src/contour/window/SettingsController_test.cpp src/contour/qml/ColorSchemeEditor.qml
git add src/contour/config/Config.hpp src/contour/config/Config.cpp # Step 5's contingency only; a no-op when unchanged
git commit -F - <<'EOF'
settings: edit the block-status, gutter-text and sticky-header colours

Six scheme-editor slots from one table. Each shows the colour the palette
derives while the scheme leaves it unset, and is written only when edited,
so saving a scheme never pins a derived colour it did not choose.

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

---

### Task 10.6: The bindings viewer shows a mouse binding's click count

The page lists bindings read-only (`SettingsController::refresh`, `:1233-1247`). A click-count binding
the user writes — say a Ctrl+triple-click `SelectCommandBlock` row — would otherwise read exactly like
the existing `Ctrl+Left → FollowHyperlink` row beside it. (Phase 5's own Ctrl+triple-click is a built-in
fallback, `builtinFallbackMouseMappings()`, not a row of the configuration, so the test below binds one
explicitly.)

**Files:**
- Modify: `src/contour/window/SettingsController.cpp` (`keybindingRow`, `:1113-1127`)
- Test: `src/contour/window/SettingsController_test.cpp` (after `TEST_CASE("SettingsController: exposes the configured keybindings read-only"`)

**Interfaces:**
- Consumes: C5 `MouseInputMapping` click count, `std::optional<uint8_t> clickCount`; the `clicks:` YAML field of an `input_mapping` mouse row (phase 5 Task 5.6) — e.g. `{ mods: [Control], mouse: Left, clicks: 3, action: SelectCommandBlock, target: pointer }`, which phase 5 ships as a built-in fallback rather than a default row.
- Produces: a mouse row's `trigger` ends in ` ×N` when the binding names a click count (e.g. `Control+Left ×3`).

- [ ] **Step 1: Write the failing test**

```cpp
TEST_CASE("SettingsController: the bindings viewer shows a mouse binding's click count", "[settings]")
{
    // Bound explicitly: phase 5's Ctrl+triple-click is a built-in fallback, never a configuration row.
    auto fx = Fixture(R"(
default_profile: main
profiles:
    main:
        show_title_bar: true
input_mapping:
    - { mods: [Control], mouse: Left, action: FollowHyperlink }
    - { mods: [Control], mouse: Left, clicks: 3, action: SelectCommandBlock, target: pointer, part: output }
)");
    auto rows = 0;
    for (auto const& raw: fx.controller->keybindings())
    {
        auto const row = raw.toMap();
        if (!row.value("action").toString().contains("SelectCommandBlock"))
            continue;
        INFO("trigger: " << row.value("trigger").toString().toStdString());
        CHECK(row.value("trigger").toString().endsWith(QStringLiteral(" ×3")));
        ++rows;
    }
    CHECK(rows > 0); // the configuration above binds Ctrl+triple-click
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run:
```bash
cmake --build --preset clangcl-debug --target contour_gui_test
out/build/clangcl-debug/bin/contour_gui_test.exe "SettingsController: the bindings viewer shows a mouse binding's click count"
```
Expected: FAIL on `endsWith(" ×3")` (the trigger reads `Control+Left`).

- [ ] **Step 3: Implement**

Replace the trigger line in `keybindingRow`:

Before:
```cpp
        auto const mods = formatModifiers(binding.modifiers);
        auto row = QVariantMap {};
        row[QStringLiteral("trigger")] = mods.isEmpty() ? inputLabel : mods + '+' + inputLabel;
```
After:
```cpp
        auto const mods = formatModifiers(binding.modifiers);
        auto trigger = mods.isEmpty() ? inputLabel : mods + '+' + inputLabel;
        // A mouse binding may name a click count (Ctrl+triple-click selects a command's output); without
        // it the row would read exactly like the plain Ctrl+click binding beside it.
        if constexpr (requires { binding.clickCount; })
            if (binding.clickCount)
                trigger += QString::fromStdString(std::format(" ×{}", static_cast<int>(*binding.clickCount)));
        auto row = QVariantMap {};
        row[QStringLiteral("trigger")] = trigger;
```

- [ ] **Step 4: Run the test to verify it passes**

Run:
```bash
clang-format -i src/contour/window/SettingsController.cpp src/contour/window/SettingsController_test.cpp
cmake --build --preset clangcl-debug --target contour_gui_test
out/build/clangcl-debug/bin/contour_gui_test.exe "[settings]"
```
Expected: PASS. If it still fails, phase 5 did not put `clickCount` on the mouse mapping as C5 says (`grep -n "clickCount" src/contour/config/Config.hpp src/vtbackend/input/InputBinding.hpp`); read it where it actually lives rather than adding a second field.

- [ ] **Step 5: Commit**

```bash
git add src/contour/window/SettingsController.cpp src/contour/window/SettingsController_test.cpp
git commit -F - <<'EOF'
settings: show a mouse binding's click count in the bindings viewer

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

---

### Task 10.7: The generated config and the reference docs carry every key

The website's configuration reference is generated, not written: CI runs `contour documentation
configuration global|profile` (`.github/workflows/docs.yml:80-84`), which renders the `*Web` strings of
`ConfigDocumentation.hpp`, and `contour documentation keys` renders `actionCatalog()`
(`ContourApp.cpp:367-394`) — a table every action must have a row in (`Actions.hpp:803-807`), whose
documentation `Actions_test.cpp:133-150` already requires to be non-empty. So the reference pages are
complete exactly when this test passes; it pins what spec §11.1 and §14 ("docs/generated config list
every key") require of phases 1–8 — and that both state the scrollbar's new default, `Right` (spec §6.1,
phase 8 Task 8.6a), which the generated `profiles.md` is the only page to document: none of the
hand-written pages under `docs/configuration/` mentions the scrollbar (Step 2 re-checks).

**Files:**
- Test: `src/contour/config/Config_test.cpp` (append after the Task 10.1 tests)
- Modify (only if Step 2 finds a gap): `src/contour/config/ConfigDocumentation.hpp` (a missing `…Config` / `…Web` entry), `src/contour/config/Config.cpp` (a missing commented colour-slot example)

**Interfaces:**
- Consumes: `std::string defaultConfigString()`, `std::string documentationGlobalConfig()`, `std::string documentationProfileConfig()` (`Config.hpp:2404-2409`).
- Produces: nothing.

- [ ] **Step 1: Write the test**

Append to `Config_test.cpp`:
```cpp
namespace
{

/// Whether @p node has a defined child at @p path, walked one segment at a time: yaml-cpp throws on
/// indexing below a missing node, so a chained lookup could not report a missing section.
[[nodiscard]] bool hasPath(YAML::Node const& node, std::span<std::string const> path)
{
    if (path.empty())
        return node.IsDefined();
    if (!node.IsDefined() || !node.IsMap())
        return false;
    return hasPath(node[path.front()], path.subspan(1));
}

} // namespace

TEST_CASE("Config: the generated config and the reference docs carry every command-block key", "[config]")
{
    // A key that `contour generate config` does not write, or the website does not document, is a key
    // nobody discovers. Each section's leaves are listed so a dropped one names itself.
    auto const text = contour::config::defaultConfigString();
    auto const generated = YAML::Load(text);
    auto const defaults = contour::config::Config {};
    auto const profile = generated["profiles"][defaults.defaultProfileName.value()];

    auto const globalPaths = std::vector<std::vector<std::string>> {
        { "gutter", "exit_status" },         { "gutter", "user_marks" },
        { "gutter", "line_numbers" },        { "gutter", "line_number_width" },
        { "gutter", "timestamps" },          { "gutter", "timestamp_format" },
        { "command_blocks", "max_records" }, { "command_blocks", "pager" },
        { "folding", "enabled" },            { "folding", "show_markers" },
        { "folding", "auto_collapse_on_new_command" }, { "folding", "on_jump_into_fold" },
    };
    auto const profilePaths = std::vector<std::vector<std::string>> {
        { "scrollbar", "marks" },
        { "sticky_header", "mode" },
        { "sticky_header", "show_evicted" },
        { "notify_on_command_finish", "when" },
        { "notify_on_command_finish", "min_duration" },
        { "notify_on_command_finish", "outcome" },
        { "notify_on_command_finish", "action" },
        { "notify_on_command_finish", "clear_on" },
    };
    for (auto const& path: globalPaths)
    {
        INFO("generated config, global: " << path[0] << "." << path[1]);
        CHECK(hasPath(generated, path));
    }
    for (auto const& path: profilePaths)
    {
        INFO("generated config, profile: " << path[0] << "." << path[1]);
        CHECK(hasPath(profile, path));
    }

    // The colour-scheme slots are optional, so the generated scheme carries them as commented examples
    // (the fold_marker rule) -- present in the text, absent from the parsed tree.
    auto const schemes = text.substr(text.find("\ncolor_schemes:"));
    for (auto const* key: { "block_status", "gutter_text", "sticky_header" })
    {
        INFO("generated colour scheme: " << key);
        CHECK(schemes.contains(key));
    }

    auto const globalDoc = contour::config::documentationGlobalConfig();
    for (auto const* key: { "gutter",
                            "exit_status",
                            "user_marks",
                            "line_numbers",
                            "line_number_width",
                            "timestamps",
                            "timestamp_format",
                            "command_blocks",
                            "max_records",
                            "pager" })
    {
        INFO("global reference: " << key);
        CHECK(globalDoc.contains(key));
    }
    auto const profileDoc = contour::config::documentationProfileConfig();
    for (auto const* key: { "marks",
                            "sticky_header",
                            "show_evicted",
                            "notify_on_command_finish",
                            "min_duration",
                            "outcome",
                            "clear_on" })
    {
        INFO("profile reference: " << key);
        CHECK(profileDoc.contains(key));
    }

    // The scrollbar is shown on the right by default (phase 8, Task 8.6a): the generated config spells the
    // default out, and the profile reference -- docs/configuration/profiles.md -- shows and states it.
    CHECK(profile["scrollbar"]["position"].as<std::string>() == "Right");
    CHECK(profileDoc.contains("position: Right"));
    CHECK(profileDoc.contains("The default is Right"));
}
```
Add `#include <span>` to the includes if absent.

- [ ] **Step 2: Run it**

Run:
```bash
clang-format -i src/contour/config/Config_test.cpp
cmake --build --preset clangcl-debug --target contour_gui_test
out/build/clangcl-debug/bin/contour_gui_test.exe "Config: the generated config and the reference docs carry every command-block key"
```
Expected: PASS — phases 1, 4–8 added these. A FAIL names the missing key: add it to that struct's `…Config` (generated YAML, with `{comment}` lines) and `…Web` (reference text) entries in `src/contour/config/ConfigDocumentation.hpp`, following `FoldingConfig` / `FoldingWeb` (`:371-391`, `:2108-2139`) — or, for a colour slot, the commented example the `fold_marker` writer emits (`Config.cpp:3932-3933`) — run `clang-format -i` on each file you changed, then re-run until it passes. A FAIL on one of the three scrollbar checks means Task 8.6a's default or its `ScrollbarWeb` text is missing: apply phase 8 Task 8.6a Steps 3–4, then re-run.

Then confirm no hand-written configuration page states a scrollbar default the generated reference now contradicts:
```bash
grep -rn -i "scrollbar" docs/configuration docs/index.md docs/features.md
```
Expected: no output (the scrollbar is documented only in the generated `docs/configuration/profiles.md`, `docs.yml:82-83`). A hit that says the scrollbar is hidden by default: change it to say it is shown on the right by default and that `scrollbar: { position: hidden }` turns it off, and add that file to the commit below.

- [ ] **Step 3: Commit**

```bash
git add src/contour/config/Config_test.cpp src/contour/config/ConfigDocumentation.hpp src/contour/config/Config.cpp
git commit -F - <<'EOF'
config: pin every command-block key in the generated config and docs

Also pins the scrollbar's default, Right, in the generated config and
the profile reference.

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```
(`git add` of an unchanged `ConfigDocumentation.hpp` or `Config.cpp` is a no-op: both are staged for Step 2's gap fix only.)

---

### Task 10.8: Website — the shell integration page

**Files:**
- Modify: `docs/vt-extensions/osc-133-shell-integration.md`

**Interfaces:**
- Consumes: `contour generate integration shell <bash|zsh|fish|tcsh|pwsh> to FILE` (`ContourApp.cpp:549-566,1081-1095`; `pwsh` added by phase 3); what each script sends (spec §10, phase 3); `MaxRecordedCommandLineBytes = 4096` (C1); `sanitizeCommandLine` purposes (C6).
- Produces: user documentation only.

- [ ] **Step 1: Check what phase 3 shipped and found**

Run (Git Bash):
```bash
for s in bash zsh fish tcsh pwsh; do printf '%s: ' $s; out/build/clangcl-debug/bin/contour.exe generate integration shell $s to - | grep -c "133;B"; done
for s in bash zsh fish pwsh; do printf '%s cmdline_url: ' $s; out/build/clangcl-debug/bin/contour.exe generate integration shell $s to - | grep -c "cmdline_url"; done
git log --format="%h %s%n%b" -i --grep="fish\|nushell\|conpty" "$(git merge-base origin/master HEAD)"..HEAD
```
(`contour.exe` comes from `cmake --build --preset clangcl-debug --target contour`; if that target cannot be built on this machine — README — run the same `grep -c` over the files in `src/contour/cli/shell-integration/` instead.)
Expected: every shell prints a non-zero `133;B` count; bash, zsh, fish and pwsh print a non-zero `cmdline_url` count; the log shows phase 3's verification notes (§16.1). The text below already states phase 3's findings — fish marks prompts natively from **4.0.0** (`A`, `C`, `D`), sends the command line from **4.0.1** and `B` from **4.3.0** (`no-mark-prompt` turns its marks off); Nushell **0.111** integrates natively, never sends `cmdline_url` and marks each continuation line with `133;A;k=s`; the PowerShell script wraps `PSConsoleHostReadLine` — so check the log agrees, and settle the one open fact: whether OSC 133 survives ConPTY (Task 3.1's verdict). If `pwsh` reports an unsupported shell, phase 3 held the PowerShell script back: drop the PowerShell row of both tables and the PSReadLine sentence, and use the alternative Windows paragraph given in Step 3.

- [ ] **Step 2: Add the installation section**

Insert after the intro line `This documentation describes the OSC 133 sequence used for shell integration, inspired by FinalTerm.`:

````markdown

## Installing Contour's shell integration

Contour ships a script for each shell it supports. Write it out once with
`contour generate integration shell <SHELL> to <FILE>`, then load it from your shell's startup file:

| Shell | Write the script | Load it |
|-------|------------------|---------|
| bash | `contour generate integration shell bash to ~/.config/contour/shell-integration.bash` | at the **end** of `~/.bashrc`: `source ~/.config/contour/shell-integration.bash` |
| zsh | `contour generate integration shell zsh to ~/.config/contour/shell-integration.zsh` | in `~/.zshrc`: `source ~/.config/contour/shell-integration.zsh` |
| fish | `contour generate integration shell fish to ~/.config/fish/conf.d/contour.fish` | nothing to do: fish reads `conf.d/` by itself |
| tcsh | `contour generate integration shell tcsh to ~/.config/contour/shell-integration.tcsh` | in `~/.tcshrc`: `source ~/.config/contour/shell-integration.tcsh` |
| PowerShell | `contour generate integration shell pwsh to "$(Split-Path $PROFILE)/contour-integration.ps1"` | in your `$PROFILE`: `. "$(Split-Path $PROFILE)/contour-integration.ps1"` |

Every script marks the start of the prompt (`A`), its end (`B`), the start of the output (`C`) and the
end of the command with its exit status (`D`), and reports the working directory with OSC 7. What
differs is the command line:

| Shell | Command line |
|-------|--------------|
| bash, zsh, fish, PowerShell | sent with `C` as `cmdline_url` |
| tcsh | not sent — tcsh has no hook that sees it; Contour reads it off the screen instead (see `C` below) |

The bash script carries [bash-preexec](https://github.com/rcaloras/bash-preexec), which is why it
belongs at the end of `~/.bashrc`: anything after it that rewrites `PROMPT_COMMAND` unhooks it. The
PowerShell script needs PSReadLine, which comes with PowerShell: like VS Code's integration, it wraps
`PSConsoleHostReadLine`, so the command line is reported when PSReadLine hands it back — after the
newline, before the command runs — rather than from a key handler.

### Shells that speak OSC 133 themselves

- **fish 4.0** and later mark their prompts natively: `A`, `C` and `D` from fish 4.0.0, the command
  line (`cmdline_url` on `C`) from 4.0.1, and `B` from 4.3.0; `set -Ua fish_features no-mark-prompt`
  (fish 4.0.6 and later) turns fish's own marks off. Contour's fish script detects which marks your fish
  sends and adds only what it leaves out, so no mark is sent twice; installing it is still worthwhile.
- **Nushell 0.111** and later integrate natively and need no script: make sure
  `$env.config.shell_integration.osc133` is `true` (the default). Nushell never sends the command line
  (`cmdline_url`), so Contour reads it off the screen, from the `;B` column to the start of the output.
  Nushell marks every continuation line of a multi-line command with `133;A;k=s`; Contour reads an `A`
  with `k=s` or `k=c` (a continuation prompt) or `k=r` (a right-hand prompt) as part of the prompt in
  progress, not as a new command block.

### Windows

PowerShell's marks reach Contour through the Windows console layer (ConPTY) like any other output, so
the PowerShell integration works on Windows as it does elsewhere. `cmd.exe` has no prompt hook to
integrate with; there, Contour behaves as it does without shell integration.

### What Contour builds on these marks

Folding, the gutter's exit-status column and line numbers, scrollbar marks, the sticky command header,
finish notifications, the recent-commands picker, "copy last command output" and prompt-to-prompt
navigation all read these marks — see [Command blocks](../demo/command-blocks.md). A shell that sends
none of them loses nothing it had: Contour then behaves as a terminal without shell integration always
has.
````

- [ ] **Step 3: Say what Contour does with B, C and D**

In `#### B - Prompt End`, after the bullet list under **Behavior:** (ending "…the user is about to type."), add:
```markdown

Contour's bundled integrations send `;B` at the very end of the prompt. Besides the uses below, it is
what lets Contour recover a command line that was never reported: the text between the `;B` column and
the start of the output.
```
In `#### C - Command Output Start`, replace:

Before:
```markdown
* `cmdline_url=<EncodedURL>`: Optional. Defines the command line being executed. The URL is percent-encoded.

**Behavior:**

* Notifies the terminal that command execution is starting and output will follow.
* The `cmdline_url` parameter allows the terminal to know exactly what command is being run (useful for features like "Run Recent Command").
```
After:
```markdown
* `cmdline_url=<EncodedURL>`: Optional. The command line being executed, percent-encoded (kitty's
  convention). Contour's bash, zsh, fish and PowerShell integrations send it.

**Behavior:**

* Notifies the terminal that command execution is starting and output will follow.
* Contour records the command line with the [command block](../demo/command-blocks.md): the sticky
  command header, finish notifications and the recent-commands picker show it. Without `cmdline_url`
  — tcsh, or a hand-written prompt — Contour reads it off the screen, from the `;B` column to the start
  of the output, so `;B` is worth sending even where the command line is not.
* A command line is untrusted input: anything that writes to the terminal can send one. Contour keeps
  at most 4 KiB of it, shows it with control and bidirectional-override characters replaced by a
  visible placeholder, and removes escape and control characters (other than tab and newline) before
  the picker inserts it at a prompt.
```
In `#### D - Command Finished`, after its **Behavior:** bullets, add:
```markdown
* Contour records the exit status with the command block: it colours the block's gutter column and
  scrollbar mark, labels the sticky header, and decides finish notifications set to `outcome: failure`.
```
If Step 1 found the PowerShell script held back, replace the `### Windows` paragraph with:
```markdown
Contour does not ship a PowerShell integration yet: the OSC 133 marks it would send do not survive
the Windows console layer (ConPTY) on their way to the terminal. `cmd.exe` has no prompt hook to
integrate with either; on Windows, Contour behaves as it does without shell integration.
```

- [ ] **Step 4: Check the spelling and commit**

Run: `ctest --test-dir out/build/clangcl-debug -R check_spelling --output-on-failure`
Expected: PASS (or SKIP when `typos` cannot be fetched).
```bash
git add docs/vt-extensions/osc-133-shell-integration.md
git commit -F - <<'EOF'
docs: document installing the shell integration and what it reports

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

---

### Task 10.9: Website — the Command blocks feature page

**Files:**
- Create: `docs/demo/command-blocks.md`
- Modify: `mkdocs.yml` (Features nav, after `- Line marks: demo/line-marks.md`, `:109`)
- Modify: `docs/features.md` (after the "Vertical Line Markers" line)
- Modify (only if Step 1 finds no `Ctrl+Alt+R` row): `docs/configuration/key-mapping.md` (built-in bindings table)

**Interfaces:**
- Consumes: the keys of spec §11.1; actions and their parameters from C5/C8 (`SelectCommandBlock { part, target }`, `OpenCommandOutput { target, program, placement, format }`, `ClearToPrompt`, `CopySelection { format, fallback }`, `OpenRecentCommands`) and phase 5's additions `CopyCommandBlock { target, part }` and `CopyCommandLine { target }` (Task 5.8); the bindings: <kbd>Ctrl</kbd>+triple-click (phase 5 Task 5.6), <kbd>Ctrl</kbd>+<kbd>Shift</kbd>+<kbd>G</kbd> (phase 5 Task 5.17) and <kbd>Ctrl</kbd>+<kbd>Alt</kbd>+<kbd>R</kbd> (phase 9 Task 9.10) are **built-in fallbacks** (`builtinFallbackMouseMappings()`, `builtinFallbackCharMappings()`), consulted after the user's own bindings whatever `contour.yml` says — so a `contour.yml` with its own `input_mapping` gets all three.
- Produces: `docs/demo/command-blocks.md`, linked from the shell-integration page (Task 10.8), the features list, the input-modes and line-marks pages (Task 10.10).

- [ ] **Step 1: Confirm the default bindings and action parameters the page names**

Run (WSL build from Task 10.12 not needed — the Windows tree suffices):
```bash
out/build/clangcl-debug/bin/contour.exe generate config to - | grep -E "SelectCommandBlock|OpenCommandOutput|OpenRecentCommands|ClearToPrompt|CopyCommandBlock|CopyCommandLine|fallback"
out/build/clangcl-debug/bin/contour.exe documentation keys | grep -E "SelectCommandBlock|OpenCommandOutput|OpenRecentCommands|ClearToPrompt|CopySelection|CopyCommandBlock|CopyCommandLine"
grep -nE "triple-click|Ctrl\+Shift\+G|Ctrl\+Alt\+R" docs/configuration/key-mapping.md
```
Expected: the generated `input_mapping` names none of `OpenRecentCommands`, a `clicks: 3` row or `OpenCommandOutput` — <kbd>Ctrl</kbd>+triple-click (`SelectCommandBlock`, `part: output, target: pointer`), <kbd>Ctrl</kbd>+<kbd>Shift</kbd>+<kbd>G</kbd> (`OpenCommandOutput`, `target: last`) and <kbd>Ctrl</kbd>+<kbd>Alt</kbd>+<kbd>R</kbd> (`OpenRecentCommands`) are built-in fallbacks, never written to a config, and the third command finds the first two in `key-mapping.md`'s built-in bindings table (phase 5 Tasks 5.6, 5.17). If it finds no `Ctrl+Alt+R` row there (Task 9.10 made the chord a fallback), add one after the `Ctrl+Shift+G` row — `` | `Ctrl+Alt+R` | Open the recent-commands picker (`OpenRecentCommands`; the commands of every open tab) | `` — and Step 5 stages the file. The keys table lists all seven actions — `SelectCommandBlock`, `CopyCommandBlock`, `CopyCommandLine`, `OpenCommandOutput`, `ClearToPrompt`, `CopySelection`, `OpenRecentCommands` — with a description. If phase 5 or 9 chose another chord (spec §16.1: "if free"), made a fallback a default or the other way round, or chose another YAML spelling for a parameter, use what the output shows in every place the page below names it.

- [ ] **Step 2: Write the page**

Create `docs/demo/command-blocks.md`:

````markdown
# Command blocks

With [shell integration](../vt-extensions/osc-133-shell-integration.md) installed, every command you
run becomes a **command block**: the prompt you typed it at and the output it printed. For each block
Contour remembers the command line, how it ended, when it started, how long it took and the directory
it ran in — for the last 1000 commands by default (`command_blocks.max_records`), even after their
output has scrolled out of the history.

Everything on this page is built on those records. Without shell integration — and without
[OSC 3008](../vt-extensions/osc-3008-context.md), which systemd 258 and later send on their own — there
are no blocks, and Contour behaves as it always has (except that the scrollbar is now shown by default):
no gutter marks, no scrollbar marks, no header, no notifications.

## The gutter

The strip left of the grid holds up to three columns, each switched on by configuration. Its width
depends only on the configuration, never on what is on screen — a gutter that grew would resize the
page mid-session.

| Column | Shows | Configured by | Default |
|--------|-------|---------------|---------|
| Timestamps | when each line was first written | `gutter.timestamps`, `gutter.timestamp_format` | off |
| Line numbers | absolute, relative or hybrid numbers | `gutter.line_numbers`, `gutter.line_number_width` | off |
| Block column | fold controls, exit status, user marks | `folding.show_markers`, `gutter.exit_status`, `gutter.user_marks` | on |

```yaml
gutter:
  exit_status: true
  user_marks: true
  line_numbers: hybrid
  line_number_width: 6
  timestamps: true
  timestamp_format: "%H:%M:%S"
```

### The block column

One cell per line, showing the first of these that applies:

| Mark | Where | Meaning |
|------|-------|---------|
| boxed minus / boxed plus | a prompt line | the block's output is shown / collapsed; click to toggle |
| ◆ | any other line | a user mark (`mm` in Vi normal mode) |
| `│`, `┕` | the output | the extent of the block |
| ▸ | a prompt line | the command is still running |
| ● | a prompt line | the command finished without output to fold — or, with `folding.show_markers: false`, finished at all |

A block's marks take the colour of how its command ended, from the colour scheme's
[`block_status`](../configuration/colors.md#block_status) slots: neutral for success, red for a
failure, yellow while it runs. With `gutter.exit_status: false` the column keeps the plain fold-marker
colour and draws neither ▸ nor ●; with `gutter.user_marks: false` it draws no ◆.

Hover the block column to see the block's exit code (and the signal that ended it, where OSC 3008 said
so), its duration, start time, directory and command line.

### Line numbers

`gutter.line_numbers` works like vim's `number` and `relativenumber`:

| Value | vim | Cursor line | Other lines |
|-------|-----|-------------|-------------|
| `off` | `nonu nornu` | — | — |
| `absolute` | `nu` | its number | their numbers |
| `relative` | `rnu` | `0` | distance from the cursor line |
| `hybrid` | `nu rnu` | its number | distance from the cursor line |

- An absolute number counts lines from the first line of the session, including lines that have
  since left the history, so a line keeps its number while the history scrolls away. Resizing the
  window re-wraps the lines and renumbers those below the top.
- A relative distance counts the lines you see: a collapsed block counts as one line, exactly as `j`
  and `k` do, so `12k` lands on the line labelled 12.
- The cursor is the Vi-mode cursor while Vi mode is on, and the terminal's cursor otherwise.
- `gutter.line_number_width` (3 to 10, default 6) is the column count; a longer number shows its last
  digits after `…`.

### Timestamps

`gutter.timestamps: true` shows beside each line the local time the cursor first reached it.
`gutter.timestamp_format` is a [`std::format` chrono format](https://en.cppreference.com/w/cpp/chrono/system_clock/formatter)
such as `%H:%M:%S` (the default) or `%H:%M`; a format that cannot be formatted is reported, and the
previous one is kept. A line shows its time only when it differs from the line above, so a burst of
output does not repeat the same second all the way down. The time costs no memory per line.

### Folding

A finished command's output can be collapsed to its prompt line (see
[`folding`](../configuration/index.md#folding)). A collapsed block says how much it hides —
`⋯ 1,234 lines` — after its prompt; that label is drawn over the page and is never selected or copied.
In Vi normal mode, vim's fold keys work:

| Key | Action |
|-----|--------|
| `za` | toggle the block under the cursor |
| `zo` | open it |
| `zc` | close it |
| `zM` | close every block |
| `zR` | open every block |

## Scrollbar marks

The scrollbar marks where commands are, so a failure far up the history is one glance — and one
click — away. Clicking a mark scrolls that command's prompt to the top, opening it if it was collapsed.
Marks that fall on the same pixel merge, the most important winning: a failure, then a running command,
then a user mark, then any command.

```yaml
profiles:
  main:
    scrollbar:
      marks: [failures, commands, user_marks]   # [] turns the marks off
```

The marks hide along with the scrollbar on the alternate screen.

The scrollbar is shown on the right by default, so the marks are there from the start. It floats over the
edge of the terminal without taking a column away, appears only once there is history to scroll through,
and stays out of the way of full-screen programs. To turn it off, and its marks with it:

```yaml
profiles:
  main:
    scrollbar:
      position: hidden     # left | right (the default) | hidden
```

or pick *hidden* for *Scrollbar position* on the settings page.

## Sticky command header

Scrolled back into a long output, the command that produced it stays pinned above it with its exit
status and duration — or `running 12s` while it runs. Click the header to jump to the command.

```yaml
profiles:
  main:
    sticky_header:
      mode: scrolled       # never | scrolled | always
      show_evicted: true
```

- `scrolled` (the default) shows the header only while you are scrolled back into the history;
  `always` also shows it while following live output, so a running `make` stays labelled above its
  own output; `never` turns it off.
- When a command's own line has already been evicted from the history, the header names the command
  instead — `⋯ make -j8 — earlier output evicted` — so its remaining output still says where it came
  from. It does so wherever the header would show, and at the very top of the history even with
  `mode: never`; `show_evicted: false` turns it off.

The header is drawn over the page: selection, copying and screen readers see only the page.
Its colours: [`sticky_header`](../configuration/colors.md#sticky_header).

## Acting on a block

| To | Use |
|----|-----|
| select a command's output | <kbd>Ctrl</kbd> + triple-click on it, or `SelectCommandBlock` |
| copy part of a command block — its output, its input line, or both — as plain text | `CopyCommandBlock` |
| copy a command line, safe to paste | `CopyCommandLine` |
| open the last command's output in a pager | <kbd>Ctrl</kbd>+<kbd>Shift</kbd>+<kbd>G</kbd>, or `OpenCommandOutput` |
| select, copy or page the block under the pointer | the context menu |
| clear everything above the current prompt | `ClearToPrompt` |
| copy the last command's output when nothing is selected | `CopySelection` with `fallback: last_command_output` |

```yaml
input_mapping:
  # Ctrl+triple-click: select the clicked command's output (built in; a row like this replaces it)
  - { mods: [Control], mouse: Left, clicks: 3, action: SelectCommandBlock, part: output, target: pointer }
  # Ctrl+Shift+G: the last command's output in the pager (built in; command_blocks.pager)
  - { mods: [Control, Shift], key: G, action: OpenCommandOutput, target: last }
  # Copy the last command's output, or its command line, without selecting anything
  - { mods: [Control, Alt], key: C, action: CopyCommandBlock, target: last, part: output }
  - { mods: [Control, Alt], key: L, action: CopyCommandLine, target: last }
  # Copy the selection -- or, with nothing selected, the last command's output
  - { mods: [Control, Shift], key: C, action: CopySelection, fallback: last_command_output }
```

- `SelectCommandBlock` takes `part` — `output`, `input` (the prompt with the command typed at it) or `all` — and `target`:
  `pointer` (the block under the mouse), `cursor` (the block holding the cursor) or `last` (the newest
  finished one).
- `CopyCommandBlock` takes the same `part` and `target` as `SelectCommandBlock` and copies that part
  into the clipboard as plain text. `CopyCommandLine` takes a `target` and copies that block's command
  line with the control characters that would be unsafe to paste removed, so an embedded end-of-paste
  sequence cannot make a shell run the rest. Both are also rows of the context menu (*Copy Output*,
  *Copy Command*).
- `OpenCommandOutput` takes the same `target`, plus `placement` — `split` (a new pane, the default),
  `tab`, or `detached` (piped into the program, with no pane) — `format` — `sgr` keeps the colours,
  `plain` drops them — and `program`, which defaults to `command_blocks.pager` (`less -R`, or `more`
  on Windows). For a pane, the output goes to a private temporary file, readable by you alone and
  deleted when the pane closes.
- `CopySelection`'s fallback is off unless you ask for it, so copying with nothing selected never
  replaces your clipboard with a build log by accident.
- Relative file paths in a command's output — clicked, or picked in [hint mode](hint-mode.md) — open
  relative to the directory that command ran in, not the shell's current one. Paths from a command
  that ran in a container, a VM or over ssh are not opened locally.

!!! note "New bindings and an existing `input_mapping`"

    Defining `input_mapping` replaces the default bindings (see [Key mapping](../configuration/key-mapping.md)).
    That costs you nothing here: <kbd>Ctrl</kbd>+triple-click, <kbd>Ctrl</kbd>+<kbd>Shift</kbd>+<kbd>G</kbd>
    and <kbd>Ctrl</kbd>+<kbd>Alt</kbd>+<kbd>R</kbd> (`OpenRecentCommands`, see [Recent commands](#recent-commands))
    are built-in fallbacks, not defaults, so they work whatever your `input_mapping` says — unless it
    binds the same chord itself, in which case your binding wins. `CopyCommandBlock`, `CopyCommandLine`
    and `ClearToPrompt` have no chord at all: add the bindings you want to your `input_mapping`.

## Finish notifications

When a command that took a while finishes somewhere you are not looking, Contour posts a desktop
notification — `✓ make finished`, or `✗ make failed (exit 2)` — with how long it took, where it ran and
which tab it is in. Clicking it brings that tab forward where the desktop supports it, and a newer
notification from the same tab replaces the older one.

```yaml
profiles:
  main:
    notify_on_command_finish:
      when: unfocused      # never | unfocused | hidden | always
      min_duration: 10     # seconds; quicker commands never notify
      outcome: any         # any | failure
      action: notify       # notify | bell | notify_bell
      clear_on: focus      # focus | next | never
```

| Key | Meaning |
|-----|---------|
| `when` | `unfocused`: the pane is not the one you are typing in (the default); `hidden`: its tab is not shown at all, or the window is minimised; `always`; `never` |
| `min_duration` | commands quicker than this many seconds never notify (default 10) |
| `outcome` | notify for `any` command, or only for a `failure` |
| `action` | a desktop notification, the profile's `bell`, or both |
| `clear_on` | withdraw the notification when you focus the pane (`focus`), when the next command there starts (`next`), or `never` |

It is on by default because the case it serves — a build in a background tab — is exactly when nobody
thinks to turn it on; the 10-second floor keeps ordinary commands quiet.

## Recent commands

`OpenRecentCommands` (<kbd>Ctrl</kbd>+<kbd>Alt</kbd>+<kbd>R</kbd>, or *Open Recent Commands* in the
command palette) lists the commands of every open tab: this tab's first, newest first, then the others
by recency, with identical commands folded into one row. Type to filter.

```yaml
input_mapping:
  # Ctrl+Alt+R: the recent-commands picker (built in; a row like this replaces it)
  - { mods: [Control, Alt], key: R, action: OpenRecentCommands }
```

- <kbd>Enter</kbd> inserts the command at the prompt; <kbd>Shift</kbd>+<kbd>Enter</kbd> inserts and
  runs it. Insertion goes through paste, so a shell with bracketed paste never runs a multi-line
  command early.
- It acts only at a shell prompt. In vim, `less`, or while a command runs, it says so and inserts
  nothing.
- A command line is whatever the program at the other end reported, so escape sequences and control
  characters are removed before it is inserted.

The list lives as long as the tabs do: closing a tab takes its commands with it. Your shell's own
history is the record that survives a restart.

## User marks

`mm` in Vi normal mode marks the cursor line, and clears the mark again. A user mark is drawn as ◆ in
the gutter and on the scrollbar, and `[m` / `]m` jump between marks of both kinds. Unlike a prompt, it
never starts a command block: folding and block-wise history eviction ignore it.

## Daemon sessions

All of this works in a [`contour client`](../persistent-sessions.md) session attached to a daemon: the
daemon sends the block records along with the screen. Reattaching does not replay notifications for
commands that finished while you were away.

## Settings page

Every setting on this page is on the GUI settings page: the *Gutter*, *Command blocks* and *Folding*
rows of the application settings, the profile's *Command blocks* group, and the colour scheme editor's
block-status, gutter-text and sticky-header slots.

## Configuration reference

- Global: [`gutter`](../configuration/index.md#gutter), [`command_blocks`](../configuration/index.md#command_blocks), [`folding`](../configuration/index.md#folding)
- Profile: [`scrollbar`](../configuration/profiles.md#scrollbar), [`sticky_header`](../configuration/profiles.md#sticky_header), [`notify_on_command_finish`](../configuration/profiles.md#notify_on_command_finish)
- Colour scheme: [`block_status`](../configuration/colors.md#block_status), [`gutter_text`](../configuration/colors.md#gutter_text), [`sticky_header`](../configuration/colors.md#sticky_header)
- Actions and bindings: [Key mapping](../configuration/key-mapping.md)
````

- [ ] **Step 3: Add the page to the nav and the features list**

In `mkdocs.yml`, after `    - Line marks: demo/line-marks.md`, add:
```yaml
    - Command blocks: demo/command-blocks.md
```
In `docs/features.md`, after the line beginning `:material-check-bold:{.check-mark}  [Vertical Line Markers]`, add:
```markdown
:material-check-bold:{.check-mark}  [Command blocks](demo/command-blocks.md): an exit-status gutter, line numbers, per-line timestamps, scrollbar marks and a sticky command header <br/>
:material-check-bold:{.check-mark}  [Notifications](demo/command-blocks.md#finish-notifications) when a long-running command finishes in a tab you are not looking at <br/>
:material-check-bold:{.check-mark}  [Recent-commands picker](demo/command-blocks.md#recent-commands) across all open tabs <br/>
:material-check-bold:{.check-mark}  [Shell integration](vt-extensions/osc-133-shell-integration.md) for bash, zsh, fish, tcsh and PowerShell <br/>
```

- [ ] **Step 4: Check the links and the spelling**

Run:
```bash
grep -o "](\.\./[^)#]*" docs/demo/command-blocks.md | sed 's/](\.\.\///' | sort -u | while read -r f; do [ -e "docs/$f" ] && echo "ok   $f" || echo "GEN? $f"; done
ctest --test-dir out/build/clangcl-debug -R check_spelling --output-on-failure
```
Expected: every target prints `ok` except `configuration/index.md` and `configuration/profiles.md`, which CI generates (`docs.yml:80-84`); spelling PASS. (The anchors `#gutter`, `#command_blocks`, `#sticky_header`, `#notify_on_command_finish` are the generated `### \`name\`` headings, `Config.hpp:2155-2172`.)

- [ ] **Step 5: Commit**

```bash
git add docs/demo/command-blocks.md mkdocs.yml docs/features.md
git add docs/configuration/key-mapping.md # Step 1's contingency only; a no-op when unchanged
git commit -F - <<'EOF'
docs: add the Command blocks feature page

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

---

### Task 10.10: Website — Vi keys, user marks, click counts, colour-scheme keys

**Files:**
- Modify: `docs/input-modes.md` (`:21-68`)
- Modify: `docs/demo/line-marks.md` (one sentence at the end of Task 1.6's `## Marking a line yourself` section)
- Verify only (no edit): `docs/configuration/key-mapping.md`
- Modify: `docs/configuration/colors.md` (new sections after `fold_marker`, directly before Task 7.11's `### \`sticky_header\`` section; commented lines in the complete example between the `fold_marker` block's `#         background: '#1a1716'` and Task 7.11's `# sticky_header:`)

**Interfaces:**
- Consumes: vi keys `mm`, `za`/`zo`/`zc`/`zM`/`zR`, `[m`/`]m`, `im`/`am` (spec §4.6, §5.5); mouse `clicks:` (C5); scheme keys `block_status.{success,failure,running}`, `gutter_text`, `sticky_header.{background,separator}` (C9) and their derived defaults (spec §5.2, §11.1).
- Produces: user documentation only.

- [ ] **Step 1: Vi keys**

In `docs/input-modes.md`, after `- Normal: \`gH\` (enter [hint mode](demo/hint-mode.md) — open selected match)`, add:
```markdown
- Normal: `mm` (set or clear a [user mark](demo/line-marks.md#marking-a-line-yourself) on the cursor line)
- Normal: `za` / `zo` / `zc` (toggle / open / close the [command block](demo/command-blocks.md#folding) under the cursor)
- Normal: `zM` / `zR` (close / open every command block)
```
After `- \`{\` & \`}\``, add:
```markdown
- `[m` & `]m` (previous / next mark: a prompt or a user mark)
```
After `- \`iW\`, \`aW\` - space delimited word`, add:
```markdown
- `im`, `am` - the lines between two marks, without / with the marked lines
```

- [ ] **Step 2: User marks**

Task 1.6 (Step 8) already documented `mm` in `docs/demo/line-marks.md` as `## Marking a line yourself`;
do not add a second section. At the end of that section — after its paragraph ending `decides where old
output is dropped from the scrollback.`, before `## Line marks as text objects` — add:
```markdown

A user mark is drawn as ◆ in the gutter and on the scrollbar; see [Command blocks](command-blocks.md).
```

- [ ] **Step 3: Mouse click counts — already documented, verify only**

Phase 5 (Task 5.6, Step 6) documented the `clicks:` field in `docs/configuration/key-mapping.md` when
it introduced it. Do not add a second paragraph. Run:

`git grep -nE "clicks|triple-click" docs/configuration/key-mapping.md`

Expected: exactly the paragraph and YAML example phase 5 added, plus the built-in bindings table row for
<kbd>Ctrl</kbd>+triple-click. If they are missing, stop and report — phase 5 did not land as planned.

- [ ] **Step 4: Colour-scheme keys**

In `docs/configuration/colors.md`, insert directly before the `### \`sticky_header\`` heading (Task 7.11 Step 5
documented the sticky-header slots there, before `### Palette Presets`; do not add them again):
````markdown
### `block_status`
Colours a command block's marks in the gutter's block column, and its mark on the scrollbar, by how
the command ended (see [Command blocks](../demo/command-blocks.md)). Each is a single colour, and all
three are **optional**; one left unset is derived from this scheme:

- `success` — a command that exited with 0, or whose exit status is unknown. Defaults to the fold
  marker's colour, so a successful block looks as the fold column always has.
- `failure` — a command that exited non-zero or was killed. Defaults to this scheme's normal red.
- `running` — a command that has not finished. Defaults to this scheme's normal yellow.

``` yaml
color_schemes:
  default:
    block_status:
      success: '#7e7c7c'
      failure: '#c63939'
      running: '#a0a000'
```

### `gutter_text`
The colour of the gutter's line numbers and timestamps. **Optional**; unset, it is this scheme's
foreground faded toward its background, as the resting fold markers are, so the numbers stay quieter
than the text beside them.

``` yaml
color_schemes:
  default:
    gutter_text: '#808080'
```

````
In the complete example, between the commented `fold_marker` block's last line (`        #         background: '#1a1716'`) and Task 7.11's `        # sticky_header:`, add:
```yaml
        # block_status:
        #     success: '#7e7c7c'
        #     failure: '#c63939'
        #     running: '#a0a000'
        # gutter_text: '#808080'
```

- [ ] **Step 5: Check the spelling and commit**

Run: `ctest --test-dir out/build/clangcl-debug -R check_spelling --output-on-failure`
Expected: PASS.
```bash
git add docs/input-modes.md docs/demo/line-marks.md docs/configuration/colors.md
git commit -F - <<'EOF'
docs: document fold keys, user marks and block colours

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

---

### Task 10.11: Release notes

**Files:**
- Modify: `metainfo.xml` (the first, unreleased `<release>`: today `0.7.1`, `:107-109`)

**Interfaces:**
- Consumes: the user-visible changes of phases 1–10; the `contour-workflows:add-release-note` convention (one `<li>` sentence per change, prefixed Fixes / Changes / Adds, issue in parentheses, prepended to the top of the unreleased list, fixes first, then changes, then additions).
- Produces: release notes; CI turns them into `docs/release-notes.md` (`docs.yml:33-35`).

- [ ] **Step 1: Confirm the facts the entries state**

Run:
```bash
grep -n "<release " metainfo.xml | head -3
git log --oneline "$(git merge-base origin/master HEAD)"..HEAD | wc -l
grep -n "ScrollBarPosition position {" src/contour/config/Config.hpp
```
Expected: the first match is the commented-out `x.y.z` template (`metainfo.xml:97`, inside `<!-- … -->`) — never edit it; the second is the unreleased `0.7.1` at `metainfo.xml:107` with `type="development"` (if a newer unreleased release precedes it, use that one); the scrollbar default reads `ScrollBarPosition::Right` (phase 8 Task 8.6a — if it still reads `Hidden`, stop and report: the scrollbar entry below would be false). The bindings, the fish version and PowerShell availability follow Task 10.8 Step 1 and Task 10.9 Step 1; adjust an entry wherever those steps found otherwise (and drop the PowerShell entry if phase 3 held the script back).

- [ ] **Step 2: Prepend the entries**

Insert as the first children of the `0.7.1` release's `<ul>` (`metainfo.xml:109`, not the template's `<ul>` at `:99`), directly after its `        <ul>` (matching the file's 10-space `<li>` indentation):
```xml
          <li>Fixes the settings page's "Grapheme clustering" and "Scaled text quality" rows having no effect: the override was written to settings.yml but never applied when the configuration was reloaded, so the page went on showing the old value with a Reset button for a change that never happened</li>
          <li>Fixes a multi-line command split into one command block per line in Nushell 0.111 and later, which integrates natively and marks each continuation line with OSC 133;A;k=s: an A carrying k=s or k=c (a continuation prompt) or k=r (a right-hand prompt) is now read as part of the prompt in progress. Nushell never reports the command line, so Contour reads it off the screen (#1010)</li>
          <li>Changes the scrollbar to be shown on the right by default (scrollbar.position was hidden), so the new exit-status marks on it (failed, finished and running commands, and user marks) are visible out of the box. It floats over the terminal's edge without taking a column, appears only while there is history to scroll through, and stays off the alternate screen. To hide it again, set scrollbar: { position: hidden } in the profile, or pick hidden for "Scrollbar position" on the settings page (#1010)</li>
          <li>Changes settings.yml to store an override of one key of a section (gutter, command_blocks, folding) as nested YAML, the way contour.yml spells it, and to apply it on its own: overriding gutter.line_numbers on the settings page leaves a gutter.timestamps set in contour.yml as it was (#1010)</li>
          <li>Changes Vi normal mode's mm to set a user mark rather than a prompt mark: it is drawn as ◆ in the gutter and on the scrollbar, and no longer counts as a command boundary for folding or block-atomic eviction, while [m, ]m and the im/am text objects still visit both kinds (#1010)</li>
          <li>Changes the bundled bash, zsh and fish integrations to report where the prompt ends (OSC 133;B) and the command line (cmdline_url), and tcsh to report the prompt end; with fish 4.0 or later, which marks its prompts itself (A, C and D from 4.0.0, the command line from 4.0.1, the prompt end from 4.3.0), the script adds only what fish leaves out (#1010)</li>
          <li>Adds PowerShell shell integration: <code>contour generate integration shell pwsh to FILE</code>; like VS Code's, it wraps PSReadLine's PSConsoleHostReadLine to report the command line (#1010)</li>
          <li>Adds a record of every command block — its command line, exit status, start time, duration and working directory — kept for the last command_blocks.max_records commands (1000 by default), even after their output has left the history (#1010)</li>
          <li>Adds exit-status colouring to the gutter's block column: a failed command's marks turn red and a running one's yellow (colour scheme block_status), a running command shows ▸ and a finished one with no output ●, and hovering the column shows the exit code, duration, start time, directory and command line (#1010)</li>
          <li>Adds line numbers to the gutter, off by default: gutter.line_numbers absolute, relative or hybrid, as vim's nu and rnu. Relative distances count the lines you see, so with a block collapsed 12k still lands on the line labelled 12 (#1010)</li>
          <li>Adds per-line timestamps to the gutter (gutter.timestamps, gutter.timestamp_format): the time the cursor first reached each line, at no memory cost per line (#1010)</li>
          <li>Adds a "⋯ 1,234 lines" label after a collapsed fold, and vim's fold keys za, zo, zc, zM and zR in Vi normal mode (#1010)</li>
          <li>Adds command marks on the scrollbar — failures, every command, and user marks (scrollbar.marks) — where clicking one scrolls to that command (#1010)</li>
          <li>Adds a sticky command header: scrolled back into a long output, the command that produced it stays pinned above it with its exit status and duration (sticky_header.mode), and once that command's own line has been evicted from the history it names the command instead, so the header never just disappears (sticky_header.show_evicted) (#1010)</li>
          <li>Adds actions on a command's output: Ctrl+triple-click selects it (SelectCommandBlock), CopyCommandBlock and CopyCommandLine copy its output or its command line (the latter safe to paste), Ctrl+Shift+G opens the last one in a pager (OpenCommandOutput, command_blocks.pager), ClearToPrompt clears everything above the current prompt, and CopySelection can fall back to the last command's output. Relative file paths in a command's output now open relative to the directory that command ran in. Ctrl+triple-click and Ctrl+Shift+G are built-in fallbacks, so they work even with a contour.yml that has its own input_mapping (#1010)</li>
          <li>Adds desktop notifications when a long command finishes where you cannot see it, on by default for commands of 10 seconds or more in an unfocused pane (notify_on_command_finish) (#1010)</li>
          <li>Adds a recent-commands picker across every open tab (OpenRecentCommands, Ctrl+Alt+R): Enter inserts the command at the prompt, Shift+Enter runs it, and it only ever acts at a shell prompt. Ctrl+Alt+R is a built-in binding, so it works even with a contour.yml that has its own input_mapping section, unless that section binds the chord itself (#1010)</li>
          <li>Adds the new gutter, command-block, folding, sticky-header, notification and scrollbar-mark settings, and the new colour-scheme slots, to the settings page (#1010)</li>
          <li>Adds daemon support for all of the above: it works in a <code>contour client</code> session attached to a daemon, and reattaching never replays notifications for commands that finished while detached (#1010)</li>
```

- [ ] **Step 3: Validate and commit**

Run:
```bash
python -c "import xml.dom.minidom,sys; xml.dom.minidom.parse('metainfo.xml'); print('well-formed')"
ctest --test-dir out/build/clangcl-debug -R check_spelling --output-on-failure
```
Expected: `well-formed`; spelling PASS. (`appstreamcli validate metainfo.xml`, where available, must report no new issue.)
```bash
git add metainfo.xml
git commit -F - <<'EOF'
metainfo: add the release notes for the semantic blocks work (#1010)

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

---

### Task 10.12: Parser throughput check (spec §13.1)

Spec §13.1 accepts no regression in parser throughput. The repo's own harness is `bench-headless`
(`src/vtbackend/bench-headless.cpp`): `grid` drives the whole terminal — parser, screen, the per-line
stamping this branch added — from a `MockViewPty`, `parser` drives the parser alone, and `cat`, `long`
and `sgr` select the termbench streams. Callgrind's instruction count is the regression oracle (it is
nearly deterministic); wall-clock MB/s is the corroboration. WSL (Ubuntu-24.04, clang-22) is the
machine: `bench-headless` is not built on Windows (`cmake/presets/os-windows.json:23`). Run every
`wsl` command from the Bash tool (Git Bash) with `MSYS_NO_PATHCONV=1`, so neither MSYS path conversion
nor PowerShell (memory: windows-verify-tricks) touches the `$` variables inside the single quotes.

**Files:** none committed. Measurements live under `/home/christianparpart/perf-1010` (WSL) and go into the PR description (Task 10.13).

**Interfaces:**
- Consumes: `bench-headless grid|parser [cat] [long] [sgr] [size MB]`, `bench-headless meta`; presets `clang-release` (RelWithDebInfo) and `clang-coverage`.
- Produces: the performance table of the PR description.

- [ ] **Step 1: Record the two commits**

Run (Git Bash, worktree root):
```bash
git rev-parse HEAD > out/perf-head.sha && git merge-base origin/master HEAD > out/perf-base.sha && cat out/perf-base.sha out/perf-head.sha
```
Expected: two SHAs — the branch's merge base ("before": no semantic-blocks code at all) and its tip.

- [ ] **Step 2: Check out both in WSL**

Run:
```bash
MSYS_NO_PATHCONV=1 wsl -d Ubuntu-24.04 -- bash -lc 'set -euo pipefail
P=/home/christianparpart/perf-1010
W=/mnt/d/contour/.claude/worktrees/semantic-blocks-1010/out
rm -rf "$P" && mkdir -p "$P"
for t in base head; do
  git clone --quiet --no-checkout /mnt/d/contour "$P/$t"
  git -C "$P/$t" checkout --quiet "$(tr -d "\r\n" < "$W/perf-$t.sha")"
done
git -C "$P/base" log -1 --oneline; git -C "$P/head" log -1 --oneline'
```
Expected: the two commits of Step 1. (Cloning from the main repository works because a worktree's commits live in its object database; the clone is Linux-native, so scripts have LF endings.)

- [ ] **Step 3: Fetch the embedded dependencies once**

Run:
```bash
MSYS_NO_PATHCONV=1 wsl -d Ubuntu-24.04 -- bash -lc 'set -euo pipefail; P=/home/christianparpart/perf-1010; cd "$P/head" && SYSDEP_ASSUME_YES=ON QTVER=6 ./scripts/install-deps.sh > "$P/deps.log" 2>&1; ln -s "$P/head/_deps" "$P/base/_deps"; ls "$P/head/_deps/sources" | head'
```
Expected: a list of fetched sources. If `sudo` asks for a password, run the same `install-deps.sh` line once in an interactive WSL terminal. (The branch adds no dependency, so both trees share one `_deps`.)

- [ ] **Step 4: Build `bench-headless` for both (long; run in the background)**

Run:
```bash
MSYS_NO_PATHCONV=1 wsl -d Ubuntu-24.04 -- bash -lc 'set -euo pipefail; P=/home/christianparpart/perf-1010
for t in base head; do
  (cd "$P/$t" && cmake --preset clang-release -B "$P/build-$t" -D CMAKE_CXX_COMPILER=clang++-22 -D CMAKE_C_COMPILER=clang-22 -D CONTOUR_FRONTEND_GUI=OFF -D CMAKE_DISABLE_FIND_PACKAGE_Qt6=ON -D CONTOUR_WAYLAND=OFF > "$P/configure-$t.log" 2>&1)
  cmake --build "$P/build-$t" --target bench-headless > "$P/build-$t.log" 2>&1
done
ls -l "$P"/build-*/src/vtbackend/bench-headless'
```
Expected: two binaries. Both trees must use identical flags; if the base tree does not build warning-free under clang-22, add `-D PEDANTIC_COMPILER_WERROR=OFF` to **both** and say so in the PR description.

- [ ] **Step 5: Memory per line**

Run:
```bash
MSYS_NO_PATHCONV=1 wsl -d Ubuntu-24.04 -- bash -lc 'P=/home/christianparpart/perf-1010; diff <("$P/build-base/src/vtbackend/bench-headless" meta) <("$P/build-head/src/vtbackend/bench-headless" meta) && echo "meta identical"'
```
Expected: `meta identical` (the cell and line sizes did not change — §13.1 "0 bytes per line"; `Line`'s own size is held by its `static_assert`).

- [ ] **Step 6: Instruction counts under Callgrind**

Run:
```bash
MSYS_NO_PATHCONV=1 wsl -d Ubuntu-24.04 -- bash -lc 'set -euo pipefail; P=/home/christianparpart/perf-1010
for t in base head; do for s in cat long sgr; do
  valgrind --tool=callgrind --callgrind-out-file="$P/$t-$s.callgrind" "$P/build-$t/src/vtbackend/bench-headless" grid $s size 4 > "$P/$t-$s.cg.log" 2>&1
done; done
for f in "$P"/*.callgrind; do printf "%s: " "$(basename "$f")"; callgrind_annotate "$f" | grep "PROGRAM TOTALS"; done'
```
Expected: six lines `base-cat.callgrind: <Ir> (100.0%) PROGRAM TOTALS`, … Record Ir for each `base`/`head` pair and Δ% = (head − base) / base. For the context of a non-zero Δ, compare `callgrind_annotate --inclusive=yes "$P/base-cat.callgrind" | head -40` with the head's; the stamping lives in `Screen::updateCursorIterator`.

- [ ] **Step 7: Wall-clock throughput**

Run:
```bash
MSYS_NO_PATHCONV=1 wsl -d Ubuntu-24.04 -- bash -lc 'set -euo pipefail; P=/home/christianparpart/perf-1010
for round in 1 2 3; do for t in base head; do
  "$P/build-$t/src/vtbackend/bench-headless" grid cat long sgr > "$P/wall-grid-$t-$round.log"
  "$P/build-$t/src/vtbackend/bench-headless" parser cat long sgr > "$P/wall-parser-$t-$round.log"
done; done
for f in "$P"/wall-*.log; do echo "== $(basename "$f")"; sed -n "/^Results/,\$p" "$f"; done'
```
Expected: the termbench "Results" table of each run (one row per stream, with its throughput). Base and head alternate inside each round so drift hits both. Record the median of the three rounds per stream, for `grid` and `parser`.

- [ ] **Step 8: Apply the acceptance rule**

- **Pass** when, for each of `grid cat`, `grid long` and `grid sgr`, Callgrind Δ ≤ +1.0 % (instruction counts move by a few tenths of a percent between runs, because the streams are random text), **and** each wall-clock median of `grid` and `parser` is at least 97 % of base.
- **Fail** otherwise: stop and report the table and both `--inclusive=yes` listings to the coordinator. Fixing a regression is a new task; this phase does not close until the check passes.

Keep the numbers for the PR description (Task 10.13 Step 9). No commit.

---

### Task 10.13: Phase gate and branch completion

**Files:** none of their own (fix commits from review land where the findings are); the PR description draft goes to `out/pr-body-1010.md` (git-ignored, `out/` in `.gitignore`).

**Interfaces:**
- Consumes: everything above; the README phase gate.
- Produces: a verified, reviewed branch and a PR description draft. **No push, no PR** — the owner opens it.

- [ ] **Step 1: Full Windows build and suite**

Run:
```bash
cmake --build --preset clangcl-debug
ctest --test-dir out/build/clangcl-debug --output-on-failure
```
Expected: zero warnings; every test that passed in the phase-0 baseline passes. Known environmental failures (README: `contour_gui_test` without staged Qt DLLs, `vtconformance_vttest` without `vttest -c`) are listed, not fixed. If the full build stops in `src/contour/display/*` on `yaml-cpp/emitter.h` (pre-existing on this machine), build the test targets one by one as phase 0 does. Note the `N tests passed, M failed` line.

- [ ] **Step 2: Spelling and formatting**

Run:
```bash
ctest --test-dir out/build/clangcl-debug -R check_spelling --output-on-failure
git diff --name-only --diff-filter=d "$(git merge-base origin/master HEAD)"..HEAD -- '*.cpp' '*.hpp' | xargs clang-format --dry-run --Werror
```
Expected: spelling PASS; no clang-format output.

- [ ] **Step 3: clang-tidy, coverage and TSan in WSL (long; run in the background)**

Run:
```bash
MSYS_NO_PATHCONV=1 wsl -d Ubuntu-24.04 -- bash -lc 'set -euo pipefail; P=/home/christianparpart/perf-1010
(cd "$P/head" && cmake --preset clang-coverage -B "$P/build-coverage" -D CMAKE_CXX_COMPILER=clang++-22 -D CMAKE_C_COMPILER=clang-22 -D CONTOUR_FRONTEND_GUI=OFF -D CMAKE_DISABLE_FIND_PACKAGE_Qt6=ON -D CONTOUR_WAYLAND=OFF -D CMAKE_EXPORT_COMPILE_COMMANDS=ON > "$P/configure-coverage.log" 2>&1)
cmake --build "$P/build-coverage" > "$P/build-coverage.log" 2>&1
ctest --test-dir "$P/build-coverage" --output-on-failure -j 8 > "$P/ctest-coverage.log" 2>&1 || true
tail -n 25 "$P/ctest-coverage.log"'
```
Expected: all tests pass except `vtconformance_vttest` (memory: wsl-vttest-missing-c-flag — the distro vttest has no `-c`; environmental).

Then lint every changed Qt-free translation unit (the GUI is configured off, so `src/contour/**` GUI files are not in this tree — memory: wsl-clang-tidy-22-verify; those were hand-audited per task):
```bash
git diff --name-only --diff-filter=d "$(git merge-base origin/master HEAD)"..HEAD -- '*.cpp' > out/changed-cpp.txt && wc -l out/changed-cpp.txt
MSYS_NO_PATHCONV=1 wsl -d Ubuntu-24.04 -- bash -lc 'P=/home/christianparpart/perf-1010; cd "$P/head"; while read -r f; do f=${f%$(printf "\r")}; grep -q "\"file\": \"$P/head/$f\"" "$P/build-coverage/compile_commands.json" || continue; echo "== $f"; clang-tidy-22 -p "$P/build-coverage" "$f" 2>&1 | grep -E "warning:|error:" || true; done < /mnt/d/contour/.claude/worktrees/semantic-blocks-1010/out/changed-cpp.txt'
```
Expected: a `== file` header per linted file and no `warning:`/`error:` line under any of them.

Coverage of the branch's units:
```bash
MSYS_NO_PATHCONV=1 wsl -d Ubuntu-24.04 -- bash -lc 'set -euo pipefail; P=/home/christianparpart/perf-1010; command -v gcovr > /dev/null || sudo apt-get install -y gcovr
cd "$P/head" && gcovr --root . --object-directory "$P/build-coverage" --gcov-executable "llvm-cov-22 gcov" \
  --filter "src/vtbackend/shell/" --filter "src/vtbackend/screen/Gutter" --filter "src/vtbackend/input/ClickCounter" \
  --filter "src/vtbackend/grid/" --filter "src/vthost/" --filter "src/contour/command/" \
  --txt "$P/coverage.txt" --txt-summary
sed -n "1,60p" "$P/coverage.txt"'
```
Expected: a per-file line-coverage table and a summary. Record the summary and every new file below 80 % lines (with why, if it is acceptable — e.g. a platform-only branch).

Data races on the event path (spec §14):
```bash
MSYS_NO_PATHCONV=1 wsl -d Ubuntu-24.04 -- bash -lc 'set -euo pipefail; P=/home/christianparpart/perf-1010
(cd "$P/head" && cmake --preset clang-tsan -B "$P/build-tsan" -D CMAKE_CXX_COMPILER=clang++-22 -D CMAKE_C_COMPILER=clang-22 -D CONTOUR_FRONTEND_GUI=OFF -D CMAKE_DISABLE_FIND_PACKAGE_Qt6=ON -D CONTOUR_WAYLAND=OFF > "$P/configure-tsan.log" 2>&1)
cmake --build "$P/build-tsan" --target vtbackend_test vthost_test > "$P/build-tsan.log" 2>&1
"$P/build-tsan/src/vtbackend/vtbackend_test" "[threading]" --allow-running-no-tests
"$P/build-tsan/src/vthost/vthost_test" "[threading]" --allow-running-no-tests'
```
Expected: both report all passed and no `WARNING: ThreadSanitizer` line.

- [ ] **Step 4: The generated documentation, as CI builds it**

Run:
```bash
MSYS_NO_PATHCONV=1 wsl -d Ubuntu-24.04 -- bash -lc 'set -euo pipefail; P=/home/christianparpart/perf-1010; C="$P/build-coverage/src/contour/contour"
cmake --build "$P/build-coverage" --target contour > /dev/null
"$C" documentation configuration global | grep -c -E "gutter|command_blocks"
"$C" documentation configuration profile | grep -c -E "sticky_header|notify_on_command_finish|marks"
"$C" documentation configuration profile | grep -c -E "position: Right|The default is Right"
"$C" documentation keys | grep -E "SelectCommandBlock|OpenCommandOutput|ClearToPrompt|OpenRecentCommands"'
```
Expected: three non-zero counts (the third: the scrollbar reference shows and states its default, `Right`) and four action rows with descriptions — the same commands `docs.yml:80-86` runs to build the site.

- [ ] **Step 5: Simplify the phase's diff**

Run `/simplify` over `git diff "$(cat out/phase10-start.sha)"..HEAD`. Commit its fixes (`settings: …` / `config: …` / `docs: …` subjects as they fall), rebuild `contour_gui_test`, and re-run `out/build/clangcl-debug/bin/contour_gui_test.exe "[settings]"` and `"[config]"` — PASS.

- [ ] **Step 6: Review the phase at xhigh**

Run `/code-review xhigh` on `"$(cat out/phase10-start.sha)"..HEAD` (or dispatch a review subagent with `effort: "xhigh"` over that range). Fix every confirmed finding, one commit per fix, and re-run the affected tests.

- [ ] **Step 7: Review the whole branch at xhigh**

Dispatch a review subagent with `effort: "xhigh"` over `git diff "$(git merge-base origin/master HEAD)"..HEAD` with this brief: check the branch against `docs/drafts/semantic-blocks.md` (every section of the Spec Coverage Map in `docs/drafts/semantic-blocks-plan/README.md` implemented), the Interface Contract C1–C9 (shapes as written), the README's Global Constraints, and each of the five Review Focus items (resize mid-command, alternate screen, hostile command lines, OSC 133 floods, no shell integration) — naming the test that covers each, or the gap. Fix every confirmed finding (one commit each), then re-run Steps 1–2. Record the final ctest pass count in the body of the last fix commit; if no fix was needed, it goes into the report of Step 10 instead (no empty commit).

- [ ] **Step 8: Confirm the tree is clean**

Run: `git status --short` and `git log --oneline "$(git merge-base origin/master HEAD)"..HEAD | wc -l`
Expected: no output from the first (git-excluded local files never show or get staged); the commit count of the branch.

- [ ] **Step 9: Draft the PR description**

Write `out/pr-body-1010.md` (git-ignored; never committed) with these sections, in this order, filled from this phase's measurements:

1. **Summary** — two sentences on what the branch delivers, and `Closes #1010`.
2. **What's in it** — one bullet per phase 1–10, in the user's terms (the release-note entries of Task 10.11 are the source).
3. **Performance** — the table of Task 10.12: per stream, Callgrind Ir base / head / Δ%, wall-clock median MB/s base / head / ratio for `grid` and `parser`; the `meta` result; the commands used; the verdict against the acceptance rule.
4. **Risk assessment** — at least: the `Line` layout change (zero bytes, held by `static_assert`); the per-cursor-line stores on the parser hot path (measured above); `commandBlockFinished` raised on the parser thread under `_stateMutex` and deferred to the GUI thread (TSan run of Step 3); the in-place change of the unreleased daemon protocol; `settings.yml` now nesting section keys (an older Contour reading a newer `settings.yml` ignores the nested sections, a newer one reads an old flat file unchanged); finish notifications on by default (10 s floor, unfocused panes only); the scrollbar shown on the right by default (every existing window gains it on upgrade; an overlay that takes no column and stays off the alternate screen, but covers the last column's right edge — glyphs under the resting handle, presses and hover in its strip; `position: hidden` restores the old look); `mm` no longer marking a command boundary; three new built-in fallback chords (<kbd>Ctrl</kbd>+triple-click, <kbd>Ctrl</kbd>+<kbd>Shift</kbd>+<kbd>G</kbd>, <kbd>Ctrl</kbd>+<kbd>Alt</kbd>+<kbd>R</kbd>) that reach users with their own `input_mapping` too, and so take those chords from the application running in the terminal unless the user binds them; OSC 133 through ConPTY (phase 3's finding).
5. **Coverage** — the gcovr summary and per-file table rows of Step 3 for the new files; the GUI-layer tests (`SettingsController_test`, `SettingsPageQml_test`, the session/notification tests) by name, noting their line coverage is not measurable on this machine (MSVC; WSL's distro Qt has no `Qt6GuiPrivate`).
6. **Tests** — the ctest summary lines (Windows, WSL coverage tree, TSan).
7. **Verification items (§16.1)** — each item and what the owning phase found (ConPTY, fish and Nushell, the two new chords — <kbd>Ctrl</kbd>+<kbd>Shift</kbd>+<kbd>G</kbd> and <kbd>Ctrl</kbd>+<kbd>Alt</kbd>+<kbd>R</kbd> both built-in fallbacks — the eviction-counter sites, nested YAML leaves — answered at the top of this phase).
8. **Follow-ups (§16.2)** — the four items, unchanged.

- [ ] **Step 10: Hand over**

Report to the coordinating session: the final ctest summary, the spelling result, the performance verdict with its table, the coverage summary, the list of review fixes, and the path `out/pr-body-1010.md`. State that the branch is **ready for the owner to open the PR** (e.g. with `/contour-workflows:create-pr`, using `out/pr-body-1010.md` as the body). Do not push, and do not open the PR. The coordinator records the result.
