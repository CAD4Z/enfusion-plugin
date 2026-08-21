# Third-party

## resources/enfusion.svg

The Enfusion mark, taken from the engine's own favicon at <https://enfusionengine.com/favicon.svg>
— the same symbol that stands in front of the word in the ENFUSION wordmark. The three paths are
unchanged. Two things about the frame are not: the favicon letterboxes the art inside a 160×160
square, so the `viewBox` here is cropped to the art itself, and the brand blue is replaced with
`currentColor`, because the activity bar uses the file as a mask and colours it by theme.

"Enfusion" and "DayZ", and the mark itself, are trademarks of Bohemia Interactive a.s. The mark is
used here to identify the engine this extension is built for. The extension is not affiliated with,
endorsed by, or published by Bohemia Interactive.

## jsonc-parser

The `.enf` files are read with [jsonc-parser](https://github.com/microsoft/node-jsonc-parser),
Microsoft's scanner and parser for JSON with comments — the same one the editor reads JSONC with,
so a file the editor accepts is a file the extension accepts. MIT licensed, and bundled into
`dist/extension.js` rather than shipped as a folder in the `.vsix`.
