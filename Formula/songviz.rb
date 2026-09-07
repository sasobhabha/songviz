class Songviz < Formula
  desc "Turn any song into a bar-spectrum visualizer video"
  homepage "https://github.com/YOURUSERNAME/songviz"
  url "https://github.com/YOURUSERNAME/songviz/archive/refs/tags/v1.0.0.tar.gz"
  sha256 "REPLACE_WITH_SHASUM_AFTER_TAGGING"
  license "MIT"

  depends_on "ffmpeg"

  def install
    system "make", "install", "PREFIX=#{prefix}"
  end

  test do
    system "#{bin}/songviz", "--version"
  end
end
