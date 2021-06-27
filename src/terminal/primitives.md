# Strongly Typing Primitives

## Requirements

- Size and offset are semantically different and need distinct types, too.
- Length
- Range with From and To
- Margin with Top, Left, Bottom, Right

## Challenges

C++ does not allow a convenient way of boxing primitive data types.

### Solution 1: Boxing through inheritance

```cpp
template <typename T> struct Boxed { T value; };
template <typename T> constexpr Boxed<T> operator+(Boxed<T> const& a, Boxed<T> const& b) noexcept { return Boxed<T>{a.value + b.value}; }
// ... more operator overloads

struct Length: public Boxed<unsigned>;
struct From: public Boxed<unsigned>;
struct To: public Boxed<unsigned>;
struct Range { From from; To to; };

Length length(Range r) { return Length{from.value + to.value}; } // ERROR
```

#### Pros

Easy to declare boxed types (on first sight).

#### Cons

Boxed types through inheritance MUST provide their own constructors as they
do not inherit the defaulted constructors from the base class (`Boxed<T>`),
which forces you to either use a macro to create a new boxed type
or cause a lot of redundant bloat to the source code, neither of what you
want to write nor maintain.

### Solution 2: Boxing through tagging

```cpp
template <typename T, typename Tag> struct Boxed
```

#### Pros

You need to provide the constructors only once - in the `Boxed` struct.
This is needed in order to avoid implicit conversion during construction
of boxed objects.

#### Cons

It is inconvenient to always also provide a tag type when creating a new
boxed type. This step seems redundant and unnecessary and is only
needed to work around the incapability of the language to express
unique boxed types.


# Conclusions

## Constructors and Initialization

It would be nice if the C++ language does provide a way to declare
simple boxing structs that do not require the boilerplate of explicitly
writing the constructors just to ensure that no implicit conversion can happen.

Idea:

```cpp
template <typename T>
explicit struct Boxed
{
    T value;
};

using Length = Boxed<unsigned>;
Length v {12};  // OK
v = Length{13}; // OK
v = 14;         // ERROR
```

This idea could be expanded to structs with more than one member:
```cpp
explicit struct Range { int a; int b; };
Range r {1, 2};   // OK
r = Range{3, 4};  // OK
r = {5, 6};       // ERROR
```

## Template tagging as a workaround for uniqueness

The other problem with boxing in C++ is tagging, which is required to
ensure that the boxed types are unique to the type system.
Not requiring tagging would probably encourage developers to adapt
strongly typed primitives.

With this in mind, having an `std::boxed<T>` in the standard library would
in turn make sense, too.

## Operator overloads and spaceships

Lastly, no matter what kind of boxing strategy you chose, you have
to implement a big fleet of operator overloads in order to make
using the boxed types convenient.

C++20 does provide a spaceship operator to reduce the fleet
of comparison operators that need creation.
It would be nice to have an equivalent to arithmetic operators (`+ - * /`)
at least, maybe also others (bit operations, unary not),
since all you do is forwarding.

So declaring a forwarding operator set for a boxed type would make
creating such APIs much more convenient.

```cpp
template <typename T> explicit struct Boxed { T value; }
template <typename T> operator forward Boxed;
```

The example syntax above would inform the compiler auto-construct
all available operators by forwarding each operation to their members.

If one doesn't like adding a new keyword, just omit the "forward",
or make it a non-reserved word that is still accepted in this context.

## my changes

Functions like `int sanitizeRange(int val, int low, int high);` became

```cpp
template <typename T>
T sanitizeRange(T val, T low, T high) {
    static_assert(std::is_integral_v<T>);
    // ...
}
```
