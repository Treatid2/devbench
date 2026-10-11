void server::start_stdio() {
    // An embedded DLL cannot own/cancel the host's standard-input stream.
    // DevBench exposes HTTP plus its external bridge; fail before admitting
    // unjoinable stdio work rather than pretending Stop can retire that reader.
    throw std::logic_error("Embedded DevBench does not support stdio server mode");
}
