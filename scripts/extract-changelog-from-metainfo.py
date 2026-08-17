#! /usr/bin/env python3

# Extract changelog entries from metainfo.xml file and print them in markdown to stdout

import sys
import xml.etree.ElementTree as ET

class Change:
    def __init__(self, text: str):
        self.text = text
        # TODO: We could replace #1234 with a link to the github issue / pull request

    def __str__(self) -> str:
        return f"- {self.text}"

class Section:
    """A group of changes, optionally introduced by a heading."""
    def __init__(self, title: str, changes: list[Change]):
        self.title = title
        self.changes = changes

    def __str__(self) -> str:
        joined_changelog_list = "\n".join([str(change) for change in self.changes])
        if self.title == "":
            return f"{joined_changelog_list}\n"
        return f"### {self.title}\n\n{joined_changelog_list}\n"

class Release:
    def __init__(self, version: str, date: str, summary: str, sections: list[Section]):
        self.version = version
        self.date = date if date != "" else "unreleased"
        self.summary = summary
        self.sections = sections

    def change_count(self) -> int:
        return sum(len(section.changes) for section in self.sections)

    def __str__(self) -> str:
        joined_sections = "\n".join([str(section) for section in self.sections])
        return f"## {self.version} ({self.date})\n\n{self.summary}{joined_sections}\n"

def extract_changelog_from_metainfo(metainfo_file: str) -> list[Release]:
    """
    Read metainfo.xml file and extract changelog entries
    """
    result = list[Release]()
    tree = ET.parse(metainfo_file)
    root = tree.getroot()

    def normalized(text: str) -> str:
        # Strip leading and trailing whitespaces
        stripped_lines = [line.strip() for line in text.strip().split("\n")]
        # Normalize repeative spaces (but retain newlines)
        return "\n".join([line for line in stripped_lines if len(line) > 0])

    for release in root.findall(".//releases/release"):
        version = release.attrib.get("version", "")
        date = release.attrib.get("date", "")
        sections = list[Section]()

        # Walk the description in document order: the first <p> is the release summary, every
        # later <p> introduces the section that the following <ul> belongs to.
        summary = ""
        pending_title = ""
        seen_summary = False
        for description in release.findall("description"):
            for element in description:
                if element.tag == "p" and element.text is not None:
                    if not seen_summary:
                        summary = normalized(element.text) + "\n"
                        seen_summary = True
                    else:
                        pending_title = normalized(element.text)
                elif element.tag == "ul":
                    changes = [Change(li.text) for li in element.findall("li")
                               if li.text is not None]
                    if len(changes) != 0:
                        sections.append(Section(pending_title, changes))
                    pending_title = ""
        if len(summary) != 0:
            summary = summary + "\n"

        release_entry = Release(version, date, summary, sections)
        if release_entry.change_count() == 0:
            print(f"Warning: No changes found for version {version} from {date}", file=sys.stderr)
        else:
            result.append(release_entry)

    return result

def write_releases_page(releases: list[Release], output_file: str):
    """
    Write releases to output file
    """
    with open(output_file, "w") as f:
        f.write("# Releases\n\n")
        for release in releases:
            f.write(str(release))

def main():
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} metainfo.xml output.md")
        sys.exit(1)
    metainfo_file = sys.argv[1]
    output_file = sys.argv[2]
    releases = extract_changelog_from_metainfo(metainfo_file)
    write_releases_page(releases, output_file)

if __name__ == "__main__":
    main()
