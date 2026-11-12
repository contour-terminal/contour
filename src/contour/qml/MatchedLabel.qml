// vim:syntax=qml
// A name with the filter's matched characters in bold, so the user sees WHY a row matched: the command
// palette's titles and the switcher's names. In the label's own colour, selected or not: an accent tint does
// not read on the light surface with the default accent.
//
// The whole string is HTML-escaped (a live tab title or a workspace name can contain & < >), so it is always
// valid StyledText. `matches` are UTF-16 code-unit indices (the model converts them from the fuzzy filter's
// UTF-8 byte offsets, window::byteOffsetsToUtf16), so they line up with charAt() even past a multibyte
// character.
import QtQuick
import QtQuick.Controls

Label {
    id: root

    /// The name, as plain text.
    property string plainText: ""
    /// The matched characters' UTF-16 indices, ascending; empty or null for none.
    property var matches: []

    textFormat: Text.StyledText
    text: root.highlighted(root.plainText, root.matches)

    // @p text as StyledText, escaped, with the characters at @p matches in bold.
    function highlighted(text, matches) {
        let next = 0;
        let result = "";
        for (let i = 0; i < text.length; ++i) {
            const hit = matches && next < matches.length && matches[next] === i;
            if (hit) {
                result += "<b>";
                ++next;
            }
            const c = text.charAt(i);
            if (c === "&")
                result += "&amp;";
            else if (c === "<")
                result += "&lt;";
            else if (c === ">")
                result += "&gt;";
            else
                result += c;
            if (hit)
                result += "</b>";
        }
        return result;
    }
}
