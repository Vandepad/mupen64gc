#include "CursorManager.h"
#include "InputManager.h"
#include "FocusManager.h"
#include "GraphicsGX.h"
#include "Image.h"
#include "resources.h"
#include "IPLFont.h"
#include "Frame.h"

namespace menu {

Cursor::Cursor()
		: currentFrame(0),
		  cursorList(0),
		  cursorX(0.0f),
		  cursorY(0.0f),
		  cursorRot(0.0f),
		  imageCenterX(12.0f),
		  imageCenterY(4.0f),
		  foundComponent(0),
		  hoverOverComponent(0),
		  pressed(false),
		  frameSwitch(true),
		  clearInput(true),
		  buttonsPressed(0),
		  activeChan(-1)
{
	pointerImage = new menu::Image(CursorPointTexture, 40, 56, GX_TF_RGBA8, GX_CLAMP, GX_CLAMP, GX_FALSE);
	grabImage = new menu::Image(CursorGrabTexture, 40, 44, GX_TF_RGBA8, GX_CLAMP, GX_CLAMP, GX_FALSE);
}

Cursor::~Cursor()
{
	delete pointerImage;
	delete grabImage;
}

void Cursor::updateCursor()
{
	if (hoverOverComponent) hoverOverComponent->setFocus(false);
	if(frameSwitch) 
	{
		clearInput = true;
		frameSwitch = false;
	}

#ifdef HW_RVL
	WPADData* wiiPad = Input::getInstance().getWpad();
	for (int i = 0; i < 4; i++)
	{
	//cycle through all 4 wiimotes
	//take first one pointing at screen
	//if (aimed at screen): set cursorX, cursorY, cursorRot, clear focusActive
		if(wiiPad[i].ir.valid && wiiPad[i].err == WPAD_ERR_NONE)
		{
			if(activeChan != i)
			{
				//clear previous cursor state here
				previousButtonsPressed[i] = wiiPad[i].btns_h;
				activeChan = i;
//				clearInput = false;
			}
			else
			{
				if(clearInput) 
				{
					previousButtonsPressed[i] = wiiPad[i].btns_h;
					printf("clearCursorFocus\nclearCursorFocus\nclearCursorFocus\nclearCursorFocus\nclearCursorFocus\n");
				}
				clearInput = false;
			}
			cursorX = wiiPad[i].ir.x;
			cursorY = wiiPad[i].ir.y; 
			cursorRot = wiiPad[i].ir.angle;
			buttonsPressed = (wiiPad[i].btns_h ^ previousButtonsPressed[i]) & wiiPad[i].btns_h;
			previousButtonsPressed[i] = wiiPad[i].btns_h;
			pressed = (buttonsPressed & (WPAD_BUTTON_A | WPAD_BUTTON_B)) ? true : false;
			Focus::getInstance().setFocusActive(false);
			if (hoverOverComponent) 
			{
				hoverOverComponent->setFocus(false);
				hoverOverComponent = NULL;
			}
			std::vector<CursorEntry>::iterator iteration;
			for (iteration = cursorList.begin(); iteration != cursorList.end(); iteration++)
			{
				if(	currentFrame == (*iteration).frame &&
					(cursorX > (*iteration).xRange[0]) && (cursorX < (*iteration).xRange[1]) &&
					(cursorY > (*iteration).yRange[0]) && (cursorY < (*iteration).yRange[1]))
					setCursorFocus((*iteration).comp);
			}
			if (!hoverOverComponent) setCursorFocus(currentFrame);
			return;
		}
	}
#endif
	//if not: clear cursorX, cursorY, cursorRot, set focusActive
	cursorX = cursorY = cursorRot = 0.0f;
	setCursorFocus(NULL);
	Focus::getInstance().setFocusActive(true);
	activeChan = -1;
}

void Cursor::setCursorFocus(Component* component)
{
	int buttonsDown = 0;
	int focusDirection = 0;

#ifdef HW_RVL
	if (buttonsPressed & WPAD_BUTTON_A) buttonsDown |= Focus::ACTION_SELECT;
	if (buttonsPressed & WPAD_BUTTON_B) buttonsDown |= Focus::ACTION_BACK;
#endif
	if (component) hoverOverComponent = component->updateFocus(focusDirection,buttonsDown);

}

void Cursor::drawCursor(Graphics& gfx)
{
	int width, height;
	if(cursorX > 0.0f || cursorY > 0.0f)
	{
		gfx.enableBlending(true);
		gfx.setTEV(GX_REPLACE);

		gfx.setDepth(-10.0f);
		gfx.newModelView();
		gfx.translate(-imageCenterX, -imageCenterY, 0.0f);
		gfx.rotate(cursorRot);
		gfx.translate(cursorX, cursorY, 0.0f);
		gfx.loadModelView();
		gfx.loadOrthographic();

		if(pressed)
		{
			grabImage->activateImage(GX_TEXMAP0);
			width = 40;
			height = 44;
		}
		else
		{
			pointerImage->activateImage(GX_TEXMAP0);
			width = 40;
			height = 56;
		}
		gfx.drawImage(0, 0, 0, width, height, 0.0, 1.0, 0.0, 1.0);
	}

/*	GXColor debugColor = (GXColor) {255, 100, 100, 255};
	IplFont::getInstance().drawInit(debugColor);
	char buffer[50];
	sprintf(buffer, "IR: %.2f, %.2f, %.2f",cursorX,cursorY,cursorRot);
	IplFont::getInstance().drawString((int) 320, (int) 240, buffer, 1.0, true);*/
}

void Cursor::addComponent(Frame* parentFrame, Component* component, float x1, float x2, float y1, float y2)
{
	CursorEntry entry;
	entry.frame = parentFrame;
	entry.comp = component;
	entry.xRange[0] = x1;
	entry.xRange[1] = x2;
	entry.yRange[0] = y1;
	entry.yRange[1] = y2;
	cursorList.push_back(entry);
}

void Cursor::removeComponent(Frame* parentFrame, Component* component)
{
	std::vector<CursorEntry>::iterator iter;

	for(iter = cursorList.begin(); iter != cursorList.end(); ++iter)
	{
		if((*iter).frame == parentFrame && (*iter).comp == component)
		{
			cursorList.erase(iter);
			break;
		}
	}
}

void Cursor::setCurrentFrame(Frame* frame)
{
	currentFrame = frame;
	frameSwitch = true;
}

Frame* Cursor::getCurrentFrame()
{
	return currentFrame;
}

void Cursor::clearInputData()
{
	clearInput = true;
}

void Cursor::clearCursorFocus()
{
	if (hoverOverComponent) hoverOverComponent->setFocus(false);
	cursorX = cursorY = 0.0f;
}

} //namespace menu 