(add-to-load-path (string-append (dirname (current-filename)) "/modules"))

(use-modules (bitcoin manifests))

(gui-manifest (getenv "HOST"))
