# The study corpus: 24 widely-used libraries, 12 C and 12 C++.
# `rt` is a regex selecting the SHARED LIBRARY runtime package(s) for that
# source. It is explicit rather than heuristic because several sources also
# ship command-line tools (curl, openssl, dbus-daemon) that carry no .so and
# would otherwise be downloaded and extracted for nothing.
LIBS = [
    # source,                lang,  runtime package regex
    ("glib2.0",              "c",   r"^libglib2\.0-0"),
    ("curl",                 "c",   r"^libcurl4(t64)?$"),
    ("openssl",              "c",   r"^libssl\d+(t64)?$"),
    ("sqlite3",              "c",   r"^libsqlite3-0"),
    ("libxml2",              "c",   r"^libxml2(-\d+)?(t64)?$"),
    ("dbus",                 "c",   r"^libdbus-1-3$"),
    ("libpng1.6",            "c",   r"^libpng16-16"),
    ("expat",                "c",   r"^libexpat1$"),
    ("harfbuzz",             "c",   r"^libharfbuzz0"),
    ("libgcrypt20",          "c",   r"^libgcrypt20$"),
    ("libzstd",              "c",   r"^libzstd1$"),
    ("gnutls28",             "c",   r"^libgnutls30"),

    ("icu",                  "cxx", r"^libicu\d+$"),
    ("protobuf",             "cxx", r"^libprotobuf\d+$"),
    ("re2",                  "cxx", r"^libre2-\d+$"),
    ("poppler",              "cxx", r"^libpoppler(\d+|-cpp\d+)$"),
    ("qtbase-opensource-src","cxx", r"^libqt5(core|gui|widgets|network)5"),
    ("gdal",                 "cxx", r"^libgdal\d+$"),
    ("proj",                 "cxx", r"^libproj\d+$"),
    ("exiv2",                "cxx", r"^libexiv2-\d+$"),
    ("tinyxml2",             "cxx", r"^libtinyxml2-\d+$"),
    ("fmtlib",               "cxx", r"^libfmt\d+$"),
    ("libraw",               "cxx", r"^libraw\d+$"),
    ("glibmm2.4",            "cxx", r"^libglibmm-2\.4-1"),
]

N_RELEASES = 12          # consecutive upstream releases per library -> 11 pairs

# Noise-control arm: same UPSTREAM version, different Debian revision. Any ABI
# difference reported here is packaging/compiler noise, not library evolution,
# so it measures the false-positive floor of the whole method.
CONTROL_LIBS = ["glib2.0", "curl", "openssl", "sqlite3", "expat",
                "icu", "protobuf", "poppler", "proj", "re2"]
CONTROL_PAIRS_PER_LIB = 3
