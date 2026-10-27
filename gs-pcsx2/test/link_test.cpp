// Opens PCSX2's GS on the Switch and feeds it nothing: a link and start-up check.
#include "un5gs.h"

int main()
{
	if (!un5gs::open())
		return 1;
	const unsigned char nothing[16] = {};
	un5gs::transfer(3, nothing, 1);
	un5gs::vsync(0, true);
	un5gs::close();
	return 0;
}
