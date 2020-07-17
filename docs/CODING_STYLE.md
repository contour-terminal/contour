
# Coding Style Guidelines


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

### Example

```cpp
namespace org::coding_style::naming_conventions
{
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
