(add-to-load-path (string-append (dirname (current-filename)) "/modules"))

(use-modules (gnu packages)
             ((gnu packages bash) #:select (bash-minimal))
             ((gnu packages cmake) #:select (cmake-minimal))
             ((gnu packages compression) #:select (gzip))
             ((gnu packages version-control) #:select (git-minimal)))

(packages->manifest
 (append
  (list ;; The Basics
        bash-minimal
        which
        coreutils-minimal
        ;; File(system) inspection
        grep
        findutils
        ;; File transformation
        patch
        sed
        ;; Compression and archiving
        tar
        gzip
        ;; Build tools
        cmake-minimal
        gnu-make
        ;; Git
        git-minimal)
  ((module-ref (resolve-interface '(bitcoin toolchains))
               'target-toolchain-packages)
   (getenv "HOST"))))
