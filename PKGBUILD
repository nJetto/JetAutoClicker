# Maintainer: JetAutoClicker maintainers <noreply@example.com>
#
# Local package build (no AUR upload needed):
#   make dist
#   cp build/jetautoclicker-*.tar.gz .
#   makepkg -si
#
# For an AUR/release package, replace the source line with the GitHub release
# tarball and put its checksum into sha256sums.

pkgname=jetautoclicker
pkgver=1.1.0
pkgrel=1
pkgdesc="Autoclicker for Linux with niri/sway, KDE/GNOME and X11 support"
arch=('x86_64' 'aarch64')
url="https://github.com/nJetto/JetAutoClicker"
license=('MIT')
depends=('gtk3' 'glib2' 'wayland')
makedepends=('pkgconf')
source=("$pkgname-$pkgver.tar.gz")
sha256sums=('SKIP')
install="$pkgname.install"

build() {
    cd "$pkgname-$pkgver"
    make VERSION="$pkgver"
}

package() {
    cd "$pkgname-$pkgver"
    make DESTDIR="$pkgdir" PREFIX=/usr install
    install -Dm644 LICENSE "$pkgdir/usr/share/licenses/$pkgname/LICENSE"
    install -Dm644 README.md "$pkgdir/usr/share/doc/$pkgname/README.md"
    install -Dm644 CHANGELOG.md "$pkgdir/usr/share/doc/$pkgname/CHANGELOG.md"
}
