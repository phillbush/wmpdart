# wmpdart

<p align="center">
  <img src="https://user-images.githubusercontent.com/63266536/190232818-336fc574-4f61-4922-af5d-16cb35f48fc2.png", title="demo"/>
</p>

wmpdart is a simple [mpd](https://www.musicpd.org/) client that lives on
the dock of some window managers such as [WindowMaker](https://www.windowmaker.org/)
and [Shod](https://github.com/phillbush/shod) and shows the album art of
the currently playing song.  It also shows the title of the song
scrolling from right to left.  When hovered, wmpdart shows two buttons:

* Clicking on the left button plays the previous song.
* Clicking on the right button plays the next song.
* Clicking elsewhere pause or continue the current song.

To build just run `make`.
You'll need libjpeg, libmpdclient, and the xlib and libXpm X11 libraries.
To install just run `make install`.

If you find the album art thumbnail too pixelated, that's because of
wmpdart's naive scaling algorithm.  If you want a more smooth miniature,
get `stb_image_resize.h` from [nothings/stb](https://github.com/nothings/stb)
and compile wmpdart with `-DUSE_STB`.
