(add-to-load-path (string-append (dirname (current-filename)) "/modules"))

(use-modules (bitcoin manifests)
             (bitcoin toolchains))

(build-manifest
 (getenv "HOST")
 (toolchain-packages
  (release-toolchain
   (getenv "HOST")
   (string->symbol (or (getenv "GUIX_BUILD_STAGE") "build")))))
