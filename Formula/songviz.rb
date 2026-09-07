class Songviz < Formula
  desc "Turn any song into a bar-spectrum visualizer video"
  homepage "https://github.com/sasobhabha/songviz"
  url "https://github.com/sasobhabha/songviz/archive/refs/tags/v1.0.0.tar.gz"
  sha256 "31dcb20730e85144ed18cb215dbac198391eaf44f64eedf910a46d172c608223"
  license "MIT"

  depends_on "ffmpeg"

  def install
    system "make", "install", "PREFIX=#{prefix}"
  end

  test do
    system "#{bin}/songviz", "--version"
  end
end
