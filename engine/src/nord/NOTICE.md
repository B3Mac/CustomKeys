# Nord Piano reader: origin and licence

`NordPiano.cpp` and `include/customkeys/NordPiano.h` are a C++ port of the
`.npno` reader and decoder in [OpenNord](https://github.com/simonflore/opennord)
(`src/lib/ns4/nsp.ts`, `src/lib/ns4/nsp-audio.ts`, `src/components/piano/pianoEngine.ts`),
copyright the OpenNord contributors and licensed under the
[GNU Affero General Public License v3](https://www.gnu.org/licenses/agpl-3.0.html).
These two files are therefore AGPL-3.0-or-later, and so is any CustomKeys build
that includes them. CustomKeys is for personal use; anyone who distributes a build
must offer its source under the AGPL. (JUCE's own free licence is also AGPLv3.)

## Nord audio stays on your machine

Following OpenNord's [LEGAL.md](https://github.com/simonflore/opennord/blob/main/docs/LEGAL.md):

- The reader decodes your own `.npno` files on your own computer, at load time, into memory.
- No Nord audio, decoded recordings, or bulk data taken from Nord's software belongs in
  this repository, its tests, releases or pull requests. The tests build small synthetic
  files instead. `.npno`, `.nsmp`, `.wav` and `.flac` are in `.gitignore`.
- The `.npno` format is a lossless predictive codec. Nothing here bypasses encryption
  or copy protection.

Nord and Nord Piano are trademarks of Clavia DMI AB. CustomKeys is not affiliated with them.
