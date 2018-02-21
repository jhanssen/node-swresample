{
  "targets": [
   {
      "include_dirs": [
	"<!(node -e \"require('nan')\")",
	"<!@(pkg-config libswresample --cflags-only-I | sed s/-I//g)",
	"<!@(pkg-config libavutil --cflags-only-I | sed s/-I//g)"
      ],
      "libraries": [
	"<!@(pkg-config libswresample --libs)",
	"<!@(pkg-config libavutil --libs)"
      ],
      "target_name": "swresample",
      "sources": [ "src/swresample.cpp" ],
      "cflags_cc": [ "-std=c++14" ],
      "xcode_settings": {
	"OTHER_CPLUSPLUSFLAGS": [
	  "-std=c++14"
	]
      }
    }
  ]
}