(define-module (bitcoin manifests)
  #:use-module (gnu packages)
  #:use-module (gnu packages base)
  #:use-module (gnu packages bison)
  #:use-module ((gnu packages bash) #:select (bash-minimal))
  #:use-module ((gnu packages cmake) #:select (cmake-minimal))
  #:use-module ((gnu packages compression) #:select (gzip xz zip))
  #:use-module (gnu packages gawk)
  #:use-module ((gnu packages installers) #:select (nsis-x86_64))
  #:use-module (gnu packages ninja)
  #:use-module (gnu packages pkg-config)
  #:use-module ((gnu packages python) #:select (python-minimal))
  #:use-module ((gnu packages python-xyz) #:select (python-lief))
  #:use-module ((gnu packages version-control) #:select (git-minimal))
  #:use-module (guix profiles)
  #:export (build-manifest
            gui-manifest))

(define (build-manifest target toolchain-packages)
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
    toolchain-packages)))

(define (gui-manifest target)
  (packages->manifest
   (append
    (list ;; Compression and archiving
          xz
          ;; Build tools
          ninja
          ;; Packaging scripts
          python-minimal ;; (3.11)
          ;; Tests
          python-lief)
    (cond ((string-suffix? "-mingw32" target)
           (list zip
                 nsis-x86_64))
          ((string-contains target "-linux-")
           (list bison
                 gawk
                 pkg-config))
          ((string-contains target "darwin")
           (list zip))
          (else '())))))
