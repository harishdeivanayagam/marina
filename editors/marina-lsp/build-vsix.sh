#!/bin/sh
# Build marina-lsp-0.2.2.vsix with nothing but `zip` (no npm/vsce needed).
# A VSIX is a zip: manifest files at the root, extension contents under extension/.
set -eu
cd "$(dirname "$0")"

rm -rf build
mkdir -p build/extension/syntaxes build/extension/fileicons/icons

cp package.json extension.js language-configuration.json README.md wave-logo.png build/extension/
cp extension.vsixmanifest '[Content_Types].xml' build/
cp syntaxes/marina.tmLanguage.json build/extension/syntaxes/
cp fileicons/marina-wave-icon-theme.json build/extension/fileicons/
cp fileicons/icons/wave.svg build/extension/fileicons/icons/

cd build
zip -q -r ../marina-lsp-0.2.2.vsix '[Content_Types].xml' extension.vsixmanifest extension
cd ..

rm -rf build
echo "built $(pwd)/marina-lsp-0.2.2.vsix"