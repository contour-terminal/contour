# Contribution Guidelines

Many thanks for your interest in contributing to the Contour terminal emulator.

Any kind of contribution is welcome, let it be coding (feature, bugfixes), documentation, or just
bug management / community management.

If you would like to contribute but do not know how, we welcome you to join our
[Discord](https://discord.gg/ncv4pG9).
You can also see some live development on [Twitch](https://www.twitch.tv/christianparpart/).

## Some general tips:

- Please keep in mind, this project is still in its very early stages (started August 2019) and thus
  under very active development. It's not perfect nor bug-free yet.
- Please fork the main repository at [github.com/contour-terminal/contour](https://github.com/contour-terminal/contour.git)
  then git clone your repository into your preferred location.
- Always work on a branch based off the master branch, if it is a feature or a bugfix.
- Please have a look at the [coding style guidelines](internals/CODING_STYLE.md).
- When creating a pull request, please see if you can keep the commits as atomic as possible.
  That is, a single reasonably sized feature or functionality should be committed as
  one single commit. If you can branch out a some functionality, let it be a more generalized API or
  refactor of something that can stand on their own, then those deserve their own commits.
- Every new feature should ideally be unit-testable, this isn't always possible or easy for pure GUI
  tasks, but the rest should ideally get one. The plus-side of this story is, that one can test their
  own code right with the new test case.
- Bug fixes should ideally also result into an added or tweaked or fixed test case, if possible.

## Contributing to the Website

The website is living directly in the project's main repository.
We are using [MkDocs](https://squidfunk.github.io/mkdocs-material/) to create the website
and have the project well documented.

Please bear with us, we just started developing this website, it is incomplete and far from perfect.
This is why documentation and website development needs the most love and probably external contributions.

## Running the website locally

In order to contribute to the website, it's best tested locally in an iterative approach.

Have a recent Python right next to you, such as Python 3.11+ and install the module requirements:

```sh
pip install docs/requirements.txt
```

Use your editor of choice to change the content in `docs/` and Running

```sh
mkdocs serve
```

to spawn the test HTTP server at port 8080.
