

# Coding Style Guidelines

- Prefer CppCoreGuidelines if possible.
- Avoid `friend` keyword.
- Use signed integers unless the integer is holding a bit pattern.
- Use the smallest integer type that is required to hold all desired values.
- Do not cast from signed to unsigned unless you want the bit pattern, prefer to cast from unsigned
  to signed instead.

## Naming Conventions

- namespaces: `snake_case`
- types: UpperCamelCase
- temporary variable: lowerCamelCase
- private member variables: lowerCamelCase with trailing underscore
- public member variable: lowerCamelCase
- constexpr variable: UpperCamelCase
- function names: lowerCamelCase
- function parameters: lowerCamelCase with leading underscore
- Template parameter name: UpperCamelCase
- preprocessor definitions: `SCREAMING_CASE`
- east const instead of west const

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
