package org.stronghold.port;

// Generated from config/compatibility.json by tools/prepare_compat.py.
final class GameCompatibility {
    static final class Executable {
        final String edition, sha256, target;
        final int bytes, targetBytes;
        Executable(String e, String s, int b, String t, int n) { edition=e; sha256=s; bytes=b; target=t; targetBytes=n; }
    }
    static final Executable[] EXECUTABLES = {
        new Executable("stronghold", "8b581f48df4d14b65b596f3a34ca2ee37b388ed93e7b1e505cceb0b1dadbcede", 4755456, "8b581f48df4d14b65b596f3a34ca2ee37b388ed93e7b1e505cceb0b1dadbcede", 4755456),
        new Executable("stronghold", "832429eedf89bc690e7ab3358bb72d654a26b46e9124c9cee176b400ca9a9715", 4755456, "8b581f48df4d14b65b596f3a34ca2ee37b388ed93e7b1e505cceb0b1dadbcede", 4755456),
        new Executable("stronghold", "96341d7aedc4828655d0a524796ae95a68f02f9401c64ebe1ed37c0445dd3e40", 4755456, "8b581f48df4d14b65b596f3a34ca2ee37b388ed93e7b1e505cceb0b1dadbcede", 4755456),
        new Executable("stronghold", "1011982d64521ab7ea2ee7375ed85c012024877680359aace62861ec923641ae", 4755456, "8b581f48df4d14b65b596f3a34ca2ee37b388ed93e7b1e505cceb0b1dadbcede", 4755456),
        new Executable("classic", "3bb0a8c1e72331b3a30a5aa93ed94beca0081b476b04c1960e26d5b45387ac5a", 8376320, "e7c7afb39653a45e2f66d5f5c17f075749e3637f4a2ef30de4c8aca24843e69a", 8384512),
        new Executable("classic", "9a5c52f616a0a09424c1dd2ecc2dc2f056fc9f82a9d69e610c3ca61a0f60ac4c", 8384512, "e7c7afb39653a45e2f66d5f5c17f075749e3637f4a2ef30de4c8aca24843e69a", 8384512),
        new Executable("classic", "c58d1610ddb34b9f1be5669668cf4ad43462657cf497afe81a35d4c9e8ac87fd", 8384512, "e7c7afb39653a45e2f66d5f5c17f075749e3637f4a2ef30de4c8aca24843e69a", 8384512),
        new Executable("classic", "e7c7afb39653a45e2f66d5f5c17f075749e3637f4a2ef30de4c8aca24843e69a", 8384512, "e7c7afb39653a45e2f66d5f5c17f075749e3637f4a2ef30de4c8aca24843e69a", 8384512),
        new Executable("extreme", "55648e6b05d67d37a5773fe699bbb17a2d6ad4de1bb9dbded9a21caef82bd7fb", 7962624, "0c90204f7bf622b768cc1fd9ebc0ec5c6cf8e60ea84911c0fdc7ffcbb2119ba8", 7970816),
        new Executable("extreme", "424ff9aac5043471a122822e8395ce3fbc835e919638cab488f6ca8ddc144da9", 7970816, "0c90204f7bf622b768cc1fd9ebc0ec5c6cf8e60ea84911c0fdc7ffcbb2119ba8", 7970816),
        new Executable("extreme", "e06506ed68c29af699770fd2f761e004a5dd2804ae4dfca58b552dc1a610f984", 7970816, "0c90204f7bf622b768cc1fd9ebc0ec5c6cf8e60ea84911c0fdc7ffcbb2119ba8", 7970816),
        new Executable("extreme", "0c90204f7bf622b768cc1fd9ebc0ec5c6cf8e60ea84911c0fdc7ffcbb2119ba8", 7970816, "0c90204f7bf622b768cc1fd9ebc0ec5c6cf8e60ea84911c0fdc7ffcbb2119ba8", 7970816),
    };
    static Executable executable(String edition, String sha256) {
        for (Executable entry : EXECUTABLES) if (entry.edition.equals(edition) && entry.sha256.equals(sha256)) return entry;
        return null;
    }
    static final String[][] LANGUAGES = {
        {"sh.tex", "b87216dbcf2cce0a66949b8688644ea0c6ab0d617d940d7f6e56e16505a76887", "ru", "\u0420\u0443\u0441\u0441\u043a\u0438\u0439"},
        {"sh.tex", "12752bb4958117cbd6b4e1a14c3200aa6a72e5176a03b6b49f0ed884cf2484b5", "en", "English"},
        {"sh.tex", "6702e795afb58ee66a33338e92cc253263c9d6be38fcd005fb20575ff4566317", "zh-Hans", "\u7b80\u4f53\u4e2d\u6587"},
        {"sh.tex", "39b2f99f77e2c2db6329b4e58d60bb39e370220c8804fcb63f4a8b82ef0c6ca3", "fr", "Fran\u00e7ais"},
        {"sh.tex", "309d7562bf8c3b41d3b1e265b6ef905a95b5646c1e536eb119401e6912121b79", "de", "Deutsch"},
        {"sh.tex", "92bb46a2a26be0fbfd967fd611dee5f22c3a5f8915d486e13afb798e654d55b3", "it", "Italiano"},
        {"sh.tex", "5a4454cd3cde9d9cbe0326906fdba4250a1364fef159f04fc71cd558ddf2bc69", "ja", "\u65e5\u672c\u8a9e"},
        {"sh.tex", "da5534f3929189bc27d5947e14a52beb72be7e25569a520d3ada0ee9ff98618c", "pl", "Polski"},
        {"sh.tex", "381372fea3932b10ba07f7628c848797912ec8163a39e97ded614358fbb5c02e", "pt-BR", "Portugu\u00eas (Brasil)"},
        {"sh.tex", "9db60a08b434cd5d7302a40e613a821827b45f825a558d1774e75d038ad5bf86", "es", "Espa\u00f1ol"},
        {"cr.tex", "ab423bec4dab75595f9568564137584e72189bc0c95851abfe54058aa2f2ff68", "en", "English"},
        {"cr.tex", "0a16fc690ccc0067d4d9084e93afc5965f47538f07e00daf54eb39aa521b301f", "zh-Hans", "\u7b80\u4f53\u4e2d\u6587"},
        {"cr.tex", "b67d9daaa1e3e61fce82b30fc9078729c63c5eb4d3d1afd5a4e594f20f77350e", "fr", "Fran\u00e7ais"},
        {"cr.tex", "6716ffe38391012f04ce32017eb65e52eeddbeb4acc76268d726786c8bfb9dc3", "de", "Deutsch"},
        {"cr.tex", "743af08f64cab9f4834dbbff8f2100fb0390742a76e242b7f8fd1f081d1c91d7", "it", "Italiano"},
        {"cr.tex", "e353362097303bef4f8fa7272c5f2c9306912965bcbb82457cc8de7f881456fa", "ja", "\u65e5\u672c\u8a9e"},
        {"cr.tex", "08603fe6fc4b8b533d99a39215c0cb6dd8383efe957ddb045d343c17ab1ee710", "pl", "Polski"},
        {"cr.tex", "989ddcb13e27bc045ab390f42a68b71b53bdf66462753cf6a66c343d1d5e264c", "ru", "\u0420\u0443\u0441\u0441\u043a\u0438\u0439"},
        {"cr.tex", "cedf413f39fb890eb015081a70ebcf09d5d7abf591296cb92be082881e633d8a", "es", "Espa\u00f1ol"},
        {"cr.tex", "b10ad23b146de08840cf3db17c7cad782005f2c5cf70d2bd2068f44a698feac5", "uk", "\u0423\u043a\u0440\u0430\u0457\u043d\u0441\u044c\u043a\u0430"},
    };
    static String[] language(String filename, String sha256) {
        for (String[] entry : LANGUAGES) if (entry[0].equals(filename) && entry[1].equals(sha256)) return entry;
        return null;
    }
}
