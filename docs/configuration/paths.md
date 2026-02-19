# Path Handling in Configuration

Contour supports special syntax in configuration file paths to make configs portable across machines and platforms.

## Tilde Expansion

Use `~` at the start of a path to refer to the user's home directory:

``` yaml
profiles:
  main:
    shell:
      initial_working_directory: ~/workspace
```

On Linux/macOS this expands to e.g. `/home/user/workspace`, on Windows to `C:\Users\user\workspace`.

## Environment Variable Expansion

Use `${VAR_NAME}` to reference environment variables:

``` yaml
color_schemes:
  default:
    background_image:
      path: '${HOME}/Pictures/terminal-bg.png'
```

This resolves `${HOME}` to its value at config load time.

### Multiple Variables

You can use several variables in the same value:

``` yaml
profiles:
  main:
    shell:
      program: '${MY_SHELL_DIR}/${MY_SHELL_NAME}'
```

### Combining with Tilde

Environment variables are expanded **before** tilde resolution, so both syntaxes compose correctly:

``` yaml
profiles:
  main:
    shell:
      initial_working_directory: '~/${PROJECT_DIR}'
```

This first resolves `${PROJECT_DIR}` (e.g. to `workspace`), then resolves `~` to the home directory, yielding `/home/user/workspace`.

## Where Expansion Applies

The following configuration values support both `~` and `${VAR}` expansion:

| Configuration value | Example |
|---------------------|---------|
| `background_image.path` | `'${HOME}/Pictures/bg.png'` |
| `shell.program` | `'${MY_SHELL}'` |
| `shell.arguments` (each entry) | `'--config=${XDG_CONFIG_HOME}/shell.conf'` |
| `shell.initial_working_directory` | `'~/${PROJECT_DIR}'` |
| Any `std::filesystem::path` config entry | General file path values |

## Cross-Platform Configuration

A primary motivation for environment variable expansion is writing configs that work on multiple operating systems without modification:

``` yaml
# Works on both Linux and Windows — no hardcoded paths
color_schemes:
  default:
    background_image:
      path: '${HOME}/Pictures/terminal-bg.png'

profiles:
  main:
    shell:
      initial_working_directory: '${HOME}/projects'
```

## Edge Cases

- **Undefined variables** expand to an empty string and a warning is logged.
- **Malformed markers** like `${UNCLOSED` (missing closing `}`) pass through the string unchanged — no partial substitution occurs.
- **No markers** — strings without any `${...}` syntax are passed through unchanged.
