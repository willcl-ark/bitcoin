target "base" {
  context = "."
  dockerfile = "./Dockerfile.base"
  platforms = ["linux/amd64"]
  tags = ["bitcoin-core-base:latest"]
}

target "asan" {
  dockerfile = "./Dockerfile.asan"
  context = "."
  platforms = ["linux/amd64"]
  tags = ["bitcoin-core-asan:latest"]
}
