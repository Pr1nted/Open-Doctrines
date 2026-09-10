# Homebrew cask for OpenDoctrines.
#
# WHERE THIS BELONGS. Not homebrew/cask -- that repository has notability
# requirements a young indie game does not meet, and a rejected pull request
# there is a week spent for nothing. This is written for YOUR OWN TAP:
#
#     github.com/Pr1nted/homebrew-tap   ->   Casks/opendoctrines.rb
#
# A tap is a plain git repository with a Casks/ directory. No review, no
# waiting, and moving to homebrew/cask later costs nothing.
#
# QUARANTINE CANNOT BE TURNED OFF FROM IN HERE. There is no `quarantine`
# stanza; Homebrew deliberately gives cask authors no way to decide that on the
# user's behalf. Since these builds are not signed with an Apple Developer ID,
# an ordinary `brew install --cask opendoctrines` installs an app macOS then
# refuses to open. The install line to publish is therefore:
#
#     brew tap Pr1nted/tap
#     brew install --cask --no-quarantine opendoctrines
#
# That flag is the user saying they trust it, which is the same decision
# README.md asks them to make with right-click -> Open, in one line instead of
# a paragraph. Publish it WITH the flag or this packaging helps nobody.
cask "opendoctrines" do
  # Matches the published asset names: OpenDoctrines-macos-{arm64,x64}.zip
  arch arm: "arm64", intel: "x64"

  version "1.2.0a"
  sha256 arm:   "fdce92d744c08d8384dd5408b0254e259077dc695f50be6bd206a5fcbe01629c",
         intel: "adf67031c60130e1d813872692d2139ede66ac63031b44768d3d7120c791f44b"

  url "https://github.com/Pr1nted/Open-Doctrines/releases/download/v#{version}/OpenDoctrines-macos-#{arch}.zip"
  name "OpenDoctrines"
  desc "Grand strategy game about running a country"
  homepage "https://github.com/Pr1nted/Open-Doctrines"

  livecheck do
    url :url
    strategy :github_latest
  end

  depends_on macos: ">= :big_sur"

  # Both archives wrap the bundle in a directory named after the architecture.
  # Everything the game needs is inside the bundle -- data/ included, 1113 files
  # of it under Contents/ -- so the app alone is a complete install.
  app "OpenDoctrines-macos-#{arch}/OpenDoctrines.app"

  # Deliberately short. The game keeps its config, saves and AI model INSIDE the
  # bundle (m_dataDir is appDir + "../data/", so Contents/data), which means
  # removing the app already removes them. Only macOS's own leftovers remain.
  zap trash: [
    "~/Library/Saved Application State/com.opendoctrines.app.savedState",
  ]
end
