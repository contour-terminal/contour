

# Coding Style Guidelines

- Prefer CppCoreGuidelines if possible.
- Avoid `friend` keyword.
- Use signed integers unless the integer is holding a bit pattern.
- Use the smallest integer type that is required to hold all desired values.
- Do not cast from signed to unsigned unless you want the bit pattern, prefer to cast from unsigned
  to signed instead.

## Naming Conventions

These are enforced by `readability-identifier-naming` in `./.clang-tidy`, which is the single
config for the whole tree — there are no module-local overrides. Where this file and `.clang-tidy`
disagree, `.clang-tidy` wins; please fix this file.

- namespaces: `snake_case`
- types (class, struct, enum, enum constants): UpperCamelCase
- temporary variable: lowerCamelCase
- private member variables: lowerCamelCase with **leading** underscore
- public member variable: lowerCamelCase
- constexpr variable: UpperCamelCase
- function names: lowerCamelCase
- function parameters: lowerCamelCase, **no** underscore
- Template parameter name: UpperCamelCase
- preprocessor definitions: `SCREAMING_CASE`
- east const instead of west const

### Exception: names the standard library binds to by spelling

Member typedefs (`value_type`, `iterator`, `const_iterator`, `difference_type`, `reference`,
`pointer`, `iterator_category`, …), nested `iterator` classes, container methods (`begin`, `end`,
`push_back`, `emplace_back`, …) and `std::formatter`/`std::hash` specializations keep their
standard spelling — renaming them silently breaks concept satisfaction and ADL. `crispy::Ring` is
`vtbackend::Lines` and is driven through `std::rotate` and range-`for`, so its iterator surface is
load-bearing. Mark each such declaration with `// NOLINT(readability-identifier-naming)`.

### Example

```cpp
namespace org::coding_style::naming_conventions
{
    void eastConst() {
        int const a = 42;        // a is const
        int const* p = &a;       // value in p is const, p is not const.
        int const *const p = &a; // both value and p are const.
    }

    enum class Role { Employed, Unemployed };

    struct User
    {
        std::string firstname;
        std::string lastname;
        Role role;
    };

    class Actor
    {
      public:
        Actor(std::string _firstname, std::string _lastname, Role _role) :
            user_{ std::move(_firstname), std::move(_lastname), _role },
            credits_{ 0 }
        {}

        void giveOrTakeCredits(int _amount) noexcept
        {
            constexpr auto Scalar = 2;
            credits += Scalar * _amount;
        }

        std::string name() const
        {
            auto const result = user.firstname + " " + user.lastname;
            return result;
        }

      private:
        User user_;
        int credits_;
    };
}
```
