{
  "targets": [
    {
      "target_name": "tree_sitter_r_binding",
      "include_dirs": [
        "<!(node -e \"require('nan')\")",
        "src"
      ],
      "sources": [
        "src/parser.c",
        "src/scanner.cc",
        "bindings/node/binding.cc"
      ],
      "cflags_c": [
        "-g",
        "-Os",
        "-std=c99",
      ],
      "cflags_cxx": [
        "-g",
        "-Os",
      ]
    }
  ]
}
