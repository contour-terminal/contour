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

class Release:
    def __init__(self, version: str, date: str, summary: str, changes: list[Change]):
        self.version = version
        self.date = date if date != "" else "unreleased"
        self.summary = summary
        self.changes = changes

    def __str__(self) -> str:
        joined_changelog_list = "\n".join([str(change) for change in self.changes])
        return f"## {self.version} ({self.date})\n\n{self.summary}{joined_changelog_list}\n\n"

def extract_changelog_from_metainfo(metainfo_file: str) -> list[Release]:
    """
    Read metainfo.xml file and extract changelog entries
    """
    result = list[Release]()
    tree = ET.parse(metainfo_file)
    root = tree.getroot()

    for release in root.findall(".//releases/release"):
        version = release.attrib.get("version", "")
        date = release.attrib.get("date", "")
        changes = list[Change]()

        summary = ""
        # Check if we've a leading <p> tag and use this as summary
        for p in release.findall(".//p"):
            if p.text is not None:
                # Strip leading and trailing whitespaces
                text = p.text.strip()
                stripped_lines = [line.strip() for line in text.split("\n")]
                # Normalize repeative spaces (but retain newlines)
                text = "\n".join([line for line in stripped_lines if len(line) > 0])
                # Append it to summary
                summary = summary + text + "\n"
        if len(summary) != 0:
            summary = summary + "\n"

        for change in release.findall(".//ul/li"):
            if change.text is not None:
                changes.append(Change(change.text))
        if (len(changes) == 0):
            print(f"Warning: No changes found for version {version} from {date}", file=sys.stderr)
        else:
            result.append(Release(version, date, summary, changes))

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
