// lazyverilog patch: spliced into slang's source/text/SourceManager.cpp ahead of
// getColumnNumber() by CMakeLists.txt.  The line an editor addresses: a file's
// own `line directives are ignored, since the editor shows the file as written.
// An unnamed buffer (predefine text) is no file, so its directives still apply;
// lazyverilog writes one there to place an includer macro in its real file.
size_t SourceManager::getFileLineNumber(SourceLocation location) const {
    std::shared_lock lock(mutex);
    SourceLocation fileLocation = getFullyExpandedLocImpl(location, lock);
    size_t rawLineNumber = getRawLineNumber(fileLocation, lock);
    if (rawLineNumber == 0)
        return 0;

    auto info = getFileInfo(fileLocation.buffer(), lock);
    if (!std::string_view(info->data->name).starts_with("<unnamed_buffer"))
        return rawLineNumber;

    auto lineDirective = info->getPreviousLineDirective(rawLineNumber);
    if (!lineDirective)
        return rawLineNumber;
    return lineDirective->lineOfDirective + (rawLineNumber - lineDirective->lineInFile) - 1;
}

