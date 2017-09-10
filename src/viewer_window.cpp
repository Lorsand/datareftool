#include "clipboard.h"
#include "allrefs.h"
#include "commandref.h"
#include "plugin.h"
#include "string_util.h"
#include "viewer_window.h"

#include "XPLMDisplay.h"
#include "XPCWidget.h"
#include "XPStandardWidgets.h"
#include "XPLMDataAccess.h"
#include "XPLMGraphics.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <iostream>
#include <memory>
#include <set>
#include <string>

const XPLMFontID font = xplmFont_Basic;

const int bottom_row_height = 20;
const int right_col_width = 20;
const int toggle_button_width = 28;
const int title_bar_height = 20;

int last_top = -1, last_bottom = -1, last_left = -1, last_right = -1;
std::string last_search_term;
bool last_case_sensitive = false, last_regex = false;
int last_change_filter_state = 0;
int last_cr_dr_filter_state = 0;

class ViewerWindow {
	XPLMWindowID window = nullptr;
	XPWidgetID regex_toggle_button = nullptr;
	XPWidgetID case_sensitive_button = nullptr;
	XPWidgetID change_filter_button = nullptr;
	XPWidgetID cr_dr_filter_button = nullptr;
	XPWidgetID search_field = nullptr;
	XPWidgetID edit_field = nullptr;
	XPWidgetID command_button_begin = nullptr;
	XPWidgetID command_button_end = nullptr;
	XPWidgetID command_button_once = nullptr;
	XPWidgetID scroll_bar = nullptr;
	XPWidgetID custom_list = nullptr;

	int drag_start_mouse_x = 0, drag_start_mouse_y = 0;
	int drag_start_window_left = 0, drag_start_window_right = 0;
	int drag_start_window_top = 0, drag_start_window_bottom = 0;
	bool in_resize_left = false;
	bool in_resize_right = false;
	bool in_resize_bottom = false;

	static const int mouse_drag_margin = 7;
	int fontheight, fontwidth;
	int displayed_lines = 0;
	int list_start_index = 0;

	int change_filter_state = 0;	//0, 1, 2 for off, changes, only big changes
	int cr_dr_filter_state = 0;	//0 for dataref only, 1 for CR only, 2 for both

	DataRefRecord * select_edit_dataref = nullptr;
	CommandRefRecord * selected_command = nullptr;
	bool edit_modified = false;

	std::vector<RefRecord *> refs;

	void hideCommandButtons() {
		XPHideWidget(command_button_once);
		XPHideWidget(command_button_begin);
		XPHideWidget(command_button_end);
	}

	static int viewerWindowCallback(XPWidgetMessage inMessage, XPWidgetID inWidget, intptr_t inParam1, intptr_t) {
		XPMouseState_t * mouse_info = (XPMouseState_t *) inParam1;
		ViewerWindow * obj = (ViewerWindow *) XPGetWidgetProperty(inWidget, xpProperty_Object, nullptr);
		switch(inMessage) {
			case xpMsg_MouseWheel:
				obj->moveScroll(mouse_info->delta);
				return 1;
			case xpMessage_CloseButtonPushed:
				closeViewerWindow(obj);
				return 1;
			case xpMsg_MouseDown:
				XPGetWidgetGeometry(obj->window, &obj->drag_start_window_left, &obj->drag_start_window_top, &obj->drag_start_window_right, &obj->drag_start_window_bottom);
				if(mouse_info->x < obj->drag_start_window_left + mouse_drag_margin) { obj->in_resize_left = true; }
				if(mouse_info->x > obj->drag_start_window_right - mouse_drag_margin) { obj->in_resize_right = true; }
				if(mouse_info->y < obj->drag_start_window_bottom + mouse_drag_margin) { obj->in_resize_bottom = true; }

				if(obj->in_resize_left || obj->in_resize_right || obj->in_resize_bottom) {
					obj->drag_start_mouse_x = mouse_info->x;
					obj->drag_start_mouse_y = mouse_info->y;
					return 1;
				} else {
					int click_y_offset_from_list_top = obj->drag_start_window_top - mouse_info->y - title_bar_height;
					if(click_y_offset_from_list_top < 0) {	//click in title bar
						return 0;
					}
					int click_y_offset_index = click_y_offset_from_list_top / obj->fontheight;
					int displayed_list_elements = std::min<int>(obj->displayed_lines, int(obj->refs.size()));

					int click_list_index = click_y_offset_index + obj->list_start_index;

					if(click_y_offset_index < displayed_list_elements && 0 <= click_list_index) {	//click is over a real list entry
						RefRecord * record = obj->refs[click_list_index];
						if(nullptr == record) {	//clicked on a commandref. ignore.
							return 1;
						}
						const std::string name = record->getName();
						float dataref_name_width = XPLMMeasureString(font, name.c_str(), int(name.size()) + 1);
						float dataref_name_width_plus_eq = XPLMMeasureString(font, (name + "=").c_str(), int(name.size()) + 2);
						const int fontheight = obj->fontheight;

						int scroll_left, scroll_right; 
						XPGetWidgetGeometry(obj->scroll_bar, &scroll_left, nullptr, &scroll_right, nullptr);
						const int scroll_width = scroll_right - scroll_left;

						//click is over name?
						int top = obj->drag_start_window_top - title_bar_height - click_y_offset_index * fontheight;
						int bottom = obj->drag_start_window_top - title_bar_height - (1 + click_y_offset_index) * fontheight;

						int nameend_x = obj->drag_start_window_left + int(dataref_name_width);
						int valuestart_x = obj->drag_start_window_left + int(dataref_name_width_plus_eq);
						const int box_padding_x = 6;
						const int box_padding_y = 6;

						if(mouse_info->x < obj->drag_start_window_left + dataref_name_width) {
							XPSetWidgetGeometry(obj->edit_field, obj->drag_start_window_left, top, nameend_x + box_padding_x, bottom - box_padding_y);
							XPSetWidgetDescriptor(obj->edit_field, name.c_str());
							obj->setEditSelection(0, name.size());
							XPShowWidget(obj->edit_field);
							obj->hideCommandButtons();
							obj->edit_modified = false;
						} else {
							DataRefRecord * dr_record = dynamic_cast<DataRefRecord *>(record);
							CommandRefRecord * cr_record = dynamic_cast<CommandRefRecord *>(record);
							if(nullptr != dr_record) {
								const std::string value_str = dr_record->getEditString();
								obj->select_edit_dataref = dr_record;
								XPSetWidgetGeometry(obj->edit_field, valuestart_x, top, obj->drag_start_window_right - mouse_drag_margin - scroll_width + box_padding_x, bottom - box_padding_y);
								XPSetWidgetDescriptor(obj->edit_field, value_str.c_str());
								obj->setEditSelection(0, value_str.size());
								XPShowWidget(obj->edit_field);
								obj->hideCommandButtons();
								obj->edit_modified = false;
							}
							if(nullptr != cr_record) {
								obj->selected_command = cr_record;
								int right = obj->drag_start_window_right - mouse_drag_margin - scroll_width + box_padding_x;
								int left = std::max(valuestart_x, right - 180);
								XPSetWidgetGeometry(obj->command_button_once, left, top, left + 60, bottom - box_padding_y);
								XPSetWidgetGeometry(obj->command_button_begin, left + 60, top, left + 120, bottom - box_padding_y);
								XPSetWidgetGeometry(obj->command_button_end, left + 120, top, left + 180, bottom - box_padding_y);
								XPShowWidget(obj->command_button_once);
								XPShowWidget(obj->command_button_begin);
								XPShowWidget(obj->command_button_end);
								XPHideWidget(obj->edit_field);
							}
						}
						return 1;
					}
				}

				return 0;
			case xpMsg_MouseUp:
				if(!obj->in_resize_left && !obj->in_resize_right && !obj->in_resize_bottom) { break; }
				obj->in_resize_left = false;
				obj->in_resize_right = false;
				obj->in_resize_bottom = false;
				break;
			case xpMsg_MouseDrag:
				if(!obj->in_resize_left && !obj->in_resize_right && !obj->in_resize_bottom) { break; }
				int delta_x = mouse_info->x - obj->drag_start_mouse_x;
				int delta_y = mouse_info->y - obj->drag_start_mouse_y;

				int left_delta = obj->in_resize_left ? delta_x : 0;
				int right_delta = obj->in_resize_right ? delta_x : 0;
				int bottom_delta = obj->in_resize_bottom ? delta_y : 0;
				
				XPSetWidgetGeometry(obj->window, obj->drag_start_window_left + left_delta, obj->drag_start_window_top, obj->drag_start_window_right + right_delta, obj->drag_start_window_bottom + bottom_delta);
				obj->resize();
				return 1;
		}

		return 0;
	}

	static int searchFieldCallback(XPWidgetMessage inMessage, XPWidgetID inWidget, intptr_t inParam1, intptr_t) {
		ViewerWindow * obj = (ViewerWindow *) XPGetWidgetProperty(inWidget, xpProperty_Object, nullptr);
		XPKeyState_t * keystruct = (XPKeyState_t *) inParam1;
		switch(inMessage) {
			case xpMsg_DescriptorChanged:
				obj->doSearch();
				return 1;
			case xpMsg_KeyPress:
				if(keystruct->flags & 6 && keystruct->flags & xplm_DownFlag) {	//alt, command, control are down
					switch(keystruct->vkey) {
						case XPLM_VK_A:	//select all
						{
							size_t length = obj->getSearchText().size();
							obj->setSearchSelection(0, length);
							return 1;
						}
						case XPLM_VK_X:	//cut
						{
							size_t start = obj->getSearchSelectionStart();
							size_t stop = obj->getSearchSelectionStop();
							std::string search_text = obj->getSearchText();
							std::string cut_text = search_text.substr(start, stop - start);
							search_text.erase(search_text.begin() + start, search_text.begin() + stop);
							obj->setSearchText(search_text);
							obj->setSearchSelection(start, start);
							setClipboard(cut_text);
							return 1;
						}
						case XPLM_VK_C:	//copy 
						{
							size_t start = obj->getSearchSelectionStart();
							size_t stop = obj->getSearchSelectionStop();
							std::string search_text = obj->getSearchText();
							std::string cut_text = search_text.substr(start, stop - start);
							setClipboard(cut_text);
							return 1;
						}
						case XPLM_VK_V:	//paste 
						{
							std::string pasted_text = getClipboard();
							size_t start = obj->getSearchSelectionStart();
							size_t stop = obj->getSearchSelectionStop();
							std::string search_text = obj->getSearchText();
							search_text.replace(search_text.begin() + start, search_text.begin() + stop, pasted_text.begin(), pasted_text.end());
							obj->setSearchText(search_text);
							obj->setSearchSelection(start + pasted_text.size(), start + pasted_text.size());
							return 1;
						}
					}
				} else {
					switch((uint8_t) keystruct->vkey) {
						case XPLM_VK_NUMPAD_ENT:
						case XPLM_VK_ENTER:
						case XPLM_VK_RETURN:
						case XPLM_VK_TAB:
						case XPLM_VK_ESCAPE:
							obj->deselectSearchField();
							break;
					}
				}
		}
		return 0;
	}


	static int editFieldCallback(XPWidgetMessage  inMessage, XPWidgetID  inWidget, intptr_t  inParam1, intptr_t) {
		ViewerWindow * obj = (ViewerWindow *) XPGetWidgetProperty(inWidget, xpProperty_Object, nullptr);
		XPKeyState_t * keystruct = (XPKeyState_t *) inParam1;
		switch(inMessage) {
			case xpMsg_DescriptorChanged:
				//obj->doSearch();
				return 1;
			case xpMsg_KeyPress:
				if(keystruct->flags & 6 && keystruct->flags & xplm_DownFlag) {	//alt, command, control are down
					switch(keystruct->vkey) {
						case XPLM_VK_A:	//select all
						{
							size_t length = obj->getEditText().size();
							obj->setEditSelection(0, length);
							return 1;
						}
						case XPLM_VK_X:	//cut
						case XPLM_VK_C:	//copy 
						{
							size_t start = obj->getEditSelectionStart();
							size_t stop = obj->getEditSelectionStop();
							std::string search_text = obj->getEditText();
							std::string cut_text = search_text.substr(start, stop - start);
							setClipboard(cut_text);
							return 1;
						}
					}
				} else {
					uint8_t vkey = keystruct->vkey;
					uint8_t key = keystruct->key;
					switch(vkey) {
						default:
							if(nullptr == obj->select_edit_dataref || false == obj->select_edit_dataref->writable()) {
								return 1;
							} else if(std::isalpha(key) || (obj->select_edit_dataref->isInt() && key == '.')) {
								return 1;
							} else if(key == ',') {
								return obj->select_edit_dataref->isArray() ? 0 : 1;
							} else {
								obj->edit_modified = true;
								return 0;
							}

						case XPLM_VK_NUMPAD_ENT:
						case XPLM_VK_ENTER:
						case XPLM_VK_RETURN:
						case XPLM_VK_TAB:
							if(obj->saveEditField()) {
								obj->deselectEditField();
							}
							return 1;
						case XPLM_VK_ESCAPE:
							obj->deselectEditField();
							return 1;
					}
				}
		}
		return 0;
	}

	static int filterClickCallback(XPWidgetMessage  inMessage, XPWidgetID  inWidget, intptr_t, intptr_t) {
		ViewerWindow * obj = (ViewerWindow *) XPGetWidgetProperty(inWidget, xpProperty_Object, nullptr);
		switch(inMessage) {
			case xpMsg_ButtonStateChanged:
				if(inWidget == obj->change_filter_button) {
					obj->change_filter_state = (obj->change_filter_state + 1) % 3;
					obj->updateChangeButton();
				}
				if(inWidget == obj->cr_dr_filter_button) {
					obj->cr_dr_filter_state = (obj->cr_dr_filter_state + 1) % 3;
					obj->updateCrDrFilterButton();
				}
				obj->doSearch();
				return 1;
		}
		return 0;
	}

	static int commandButtonCallback(XPWidgetMessage  inMessage, XPWidgetID  inWidget, intptr_t, intptr_t) {
		ViewerWindow * obj = (ViewerWindow *) XPGetWidgetProperty(inWidget, xpProperty_Object, nullptr);
		switch(inMessage) {
			case xpMsg_PushButtonPressed:
				if(nullptr != obj->selected_command) {
					obj->selected_command->touch();
					if(inWidget == obj->command_button_once) {
						obj->selected_command->commandOnce();
					} else if(inWidget == obj->command_button_begin) {
						obj->selected_command->commandBegin();
					} else if(inWidget == obj->command_button_end) {
						obj->selected_command->commandEnd();
					}
				}
				return 1;
		}
		return 0;
	}

	static int drawListCallback(XPWidgetMessage  inMessage, XPWidgetID  inWidget, intptr_t, intptr_t) {
		ViewerWindow * obj = (ViewerWindow *) XPGetWidgetProperty(inWidget, xpProperty_Object, nullptr);
		switch(inMessage) {
			case xpMsg_Draw:
				obj->draw();
				return 1;
		}
		return 0;
	}

public:
	ViewerWindow(int l, int t, int r, int b) {
		XPLMGetFontDimensions(font, &fontwidth, &fontheight, nullptr);

		window = XPCreateWidget(l, t, r, b,
					1,										// Visible
					"Data Ref Tool",	// desc
					1,										// root
					NULL,									// no container
					xpWidgetClass_MainWindow);

		XPSetWidgetProperty(window, xpProperty_MainWindowHasCloseBoxes, 1);
		XPSetWidgetProperty(window, xpProperty_MainWindowType, xpMainWindowStyle_Translucent);
		XPAddWidgetCallback(window, viewerWindowCallback);
		XPSetWidgetProperty(window, xpProperty_Object, (intptr_t)this);

		custom_list = XPCreateCustomWidget(0, 0, 1, 1, 1,"", 0, window, drawListCallback);
		//XPAddWidgetCallback(custom_list, drawListCallback);
		XPSetWidgetProperty(custom_list, xpProperty_Object, (intptr_t)this);

		regex_toggle_button = XPCreateWidget(0, 0, 1, 1, 1,".*", 0, window, xpWidgetClass_Button);
		XPSetWidgetProperty(regex_toggle_button, xpProperty_ButtonType, xpPushButton);
		XPSetWidgetProperty(regex_toggle_button, xpProperty_ButtonBehavior, xpButtonBehaviorCheckBox);
		XPSetWidgetProperty(regex_toggle_button, xpProperty_ButtonState, 0);
		XPAddWidgetCallback(regex_toggle_button, filterClickCallback);
		XPSetWidgetProperty(regex_toggle_button, xpProperty_Object, (intptr_t)this);

		case_sensitive_button = XPCreateWidget(0, 0, 1, 1, 1,"Aa", 0, window, xpWidgetClass_Button);
		XPSetWidgetProperty(case_sensitive_button, xpProperty_ButtonType, xpPushButton);
		XPSetWidgetProperty(case_sensitive_button, xpProperty_ButtonBehavior, xpButtonBehaviorCheckBox);
		XPSetWidgetProperty(case_sensitive_button, xpProperty_ButtonState, 0);
		XPAddWidgetCallback(case_sensitive_button, filterClickCallback);
		XPSetWidgetProperty(case_sensitive_button, xpProperty_Object, (intptr_t)this);

		change_filter_button = XPCreateWidget(0, 0, 1, 1, 1,"Ch", 0, window, xpWidgetClass_Button);
		XPSetWidgetProperty(change_filter_button, xpProperty_ButtonType, xpPushButton);
		XPSetWidgetProperty(change_filter_button, xpProperty_ButtonBehavior, xpButtonBehaviorCheckBox);
		XPSetWidgetProperty(change_filter_button, xpProperty_ButtonState, 0);
		XPAddWidgetCallback(change_filter_button, filterClickCallback);
		XPSetWidgetProperty(change_filter_button, xpProperty_Object, (intptr_t)this);
		updateChangeButton();

		cr_dr_filter_button = XPCreateWidget(0, 0, 1, 1, 1,"??", 0, window, xpWidgetClass_Button);
		XPSetWidgetProperty(cr_dr_filter_button, xpProperty_ButtonType, xpPushButton);
		XPSetWidgetProperty(cr_dr_filter_button, xpProperty_ButtonBehavior, xpButtonBehaviorCheckBox);
		XPSetWidgetProperty(cr_dr_filter_button, xpProperty_ButtonState, 0);
		XPAddWidgetCallback(cr_dr_filter_button, filterClickCallback);
		XPSetWidgetProperty(cr_dr_filter_button, xpProperty_Object, (intptr_t)this);
		updateCrDrFilterButton();

		search_field = XPCreateWidget(0, 0, 1, 1, 1,"", 0, window, xpWidgetClass_TextField);
		XPSetWidgetProperty(search_field, xpProperty_TextFieldType, xpTextTranslucent);
		XPAddWidgetCallback(search_field, searchFieldCallback);
		XPSetWidgetProperty(search_field, xpProperty_Object, (intptr_t)this);

		edit_field = XPCreateWidget(0, 0, 1, 1, 1,"", 0, window, xpWidgetClass_TextField);
		//XPSetWidgetProperty(edit_field, xpProperty_TextFieldType, xpTextTranslucent);
		XPAddWidgetCallback(edit_field, editFieldCallback);
		XPSetWidgetProperty(edit_field, xpProperty_Object, (intptr_t)this);
		XPHideWidget(edit_field);

		command_button_once = XPCreateWidget(0, 0, 1, 1, 1,"Once", 0, window, xpWidgetClass_Button);
		command_button_begin = XPCreateWidget(0, 0, 1, 1, 1,"Begin", 0, window, xpWidgetClass_Button);
		command_button_end = XPCreateWidget(0, 0, 1, 1, 1,"End", 0, window, xpWidgetClass_Button);
		for(XPWidgetID button : {command_button_once, command_button_begin, command_button_end}) {
			XPSetWidgetProperty(button, xpProperty_ButtonType, xpPushButton);
			XPSetWidgetProperty(button, xpProperty_ButtonBehavior, xpButtonBehaviorPushButton);
			XPAddWidgetCallback(button, commandButtonCallback);
			XPSetWidgetProperty(button, xpProperty_Object, (intptr_t)this);
		}
		hideCommandButtons();

		scroll_bar = XPCreateWidget(0, 0, 1, 1, 1,"", 0, window, xpWidgetClass_ScrollBar);
		XPSetWidgetProperty(scroll_bar, xpProperty_ScrollBarType, xpScrollBarTypeScrollBar);	//might need changing
		XPSetWidgetProperty(scroll_bar, xpProperty_ScrollBarMin, 0);
		XPSetWidgetProperty(scroll_bar, xpProperty_ScrollBarMax, 0);

		if(last_left != -1) {
			XPSetWidgetGeometry(window, last_left, last_top, last_right, last_bottom);
			XPSetWidgetDescriptor(search_field, last_search_term.c_str());
			XPSetWidgetProperty(case_sensitive_button, xpProperty_ButtonState, last_case_sensitive ? 1 : 0);
			XPSetWidgetProperty(regex_toggle_button, xpProperty_ButtonState, last_regex ? 1 : 0);
			last_left = last_top = last_right = last_bottom = -1;
			last_search_term.clear();
			last_case_sensitive = last_regex = false;
			change_filter_state = last_change_filter_state;
			cr_dr_filter_state = last_cr_dr_filter_state;
			updateChangeButton();
			updateCrDrFilterButton();
		}

		resize();

		doSearch();
		int max_scroll = std::max<int>(0, int(refs.size() - displayed_lines));
		XPSetWidgetProperty(scroll_bar, xpProperty_ScrollBarMax, max_scroll);
		XPSetWidgetProperty(scroll_bar, xpProperty_ScrollBarSliderPosition, max_scroll);
	}

	~ViewerWindow() {
		XPGetWidgetGeometry(window, &last_left, &last_top, &last_right, &last_bottom);
		last_search_term = getSearchText();

		last_case_sensitive = 0 != XPGetWidgetProperty(case_sensitive_button, xpProperty_ButtonState, nullptr);
		last_regex = 0 != XPGetWidgetProperty(regex_toggle_button, xpProperty_ButtonState, nullptr);
		last_change_filter_state = change_filter_state;
		last_cr_dr_filter_state = cr_dr_filter_state;

		XPHideWidget(window);
		XPLMDestroyWindow(window);
	}

	void updateScroll() {
		deselectEditField();

		//update the scrollbar
		int scroll_pos = (int)XPGetWidgetProperty(scroll_bar, xpProperty_ScrollBarSliderPosition, nullptr);
		int max_scroll_pos = std::max<int>(0, int(refs.size()) - displayed_lines);
		XPSetWidgetProperty(scroll_bar, xpProperty_ScrollBarMin, 0);
		XPSetWidgetProperty(scroll_bar, xpProperty_ScrollBarMax, max_scroll_pos);
		if(scroll_pos > max_scroll_pos) {
			XPSetWidgetProperty(scroll_bar, xpProperty_ScrollBarSliderPosition, max_scroll_pos);
		}
	}

	void deselectSearchField() {
		XPLoseKeyboardFocus(search_field);
	}

	void moveScroll(int amount) {
		intptr_t scroll_pos = XPGetWidgetProperty(scroll_bar, xpProperty_ScrollBarSliderPosition, nullptr) + amount;
		int max_scroll_pos = std::max<int>(0, int(refs.size()) - displayed_lines);
		scroll_pos = std::min<intptr_t>(max_scroll_pos, std::max<intptr_t>(0, scroll_pos));

		XPSetWidgetProperty(scroll_bar, xpProperty_ScrollBarSliderPosition, scroll_pos);
		deselectEditField();
		XPHideWidget(edit_field);
		hideCommandButtons();
	}

	std::string getSearchText() const {
		char searchfield_text[1024];
		XPGetWidgetDescriptor(search_field, searchfield_text, 1024);
		return std::string(searchfield_text);
    }
    
    void setSearchText(const std::string & s) {
        XPSetWidgetDescriptor(search_field, s.c_str());
    }

	intptr_t getSearchSelectionStart() const {
		return XPGetWidgetProperty(search_field, xpProperty_EditFieldSelStart, NULL);
	}

	intptr_t getSearchSelectionStop() const {
		return XPGetWidgetProperty(search_field, xpProperty_EditFieldSelEnd, NULL);
	}

	void setSearchSelection(intptr_t start, intptr_t stop) {
		XPSetWidgetProperty(search_field, xpProperty_EditFieldSelStart, start);
		XPSetWidgetProperty(search_field, xpProperty_EditFieldSelEnd, stop);
	}

	bool saveEditField() {
		if(edit_modified) {
			const std::string edit_txt = getEditText();

			if(select_edit_dataref->isDouble()) {
				try {
					double d = std::stold(edit_txt);
					select_edit_dataref->setDouble(d);
				} catch(std::exception &) {
					return false;
				}
			} else if(select_edit_dataref->isFloat()) {
				try {
					float f = std::stof(edit_txt);
					select_edit_dataref->setFloat(f);
				} catch(std::exception &) {
					return false;
				}
			} else if(select_edit_dataref->isInt()) {
				try {
					int i = std::stoi(edit_txt);
					select_edit_dataref->setInt(i);
				} catch(std::exception &) {
					return false;
				}
			} else if(select_edit_dataref->isIntArray()) {
				std::vector<int> array;
				if(false == parseArray<int>(edit_txt, array, select_edit_dataref->getArrayLength())) {
					return false;
				}
				select_edit_dataref->setIntArray(array);
			} else if(select_edit_dataref->isFloatArray()) {
				std::vector<float> array;
				if(false == parseArray<float>(edit_txt, array, select_edit_dataref->getArrayLength())) {
					return false;
				}
				select_edit_dataref->setFloatArray(array);
			}
		}

		edit_modified = false;
		return true;
	}

	void deselectEditField() {

		edit_modified = false;
		select_edit_dataref = nullptr;
		XPLoseKeyboardFocus(edit_field);
		XPHideWidget(edit_field);
		hideCommandButtons();
	}

	std::string getEditText() const {
		char editfield_text[1024];
		XPGetWidgetDescriptor(edit_field, editfield_text, 1024);
		return std::string(editfield_text);
	}

	intptr_t getEditSelectionStart() const {
		return XPGetWidgetProperty(edit_field, xpProperty_EditFieldSelStart, NULL);
	}

	intptr_t getEditSelectionStop() const {
		return XPGetWidgetProperty(edit_field, xpProperty_EditFieldSelEnd, NULL);
	}

	void setEditSelection(intptr_t start, intptr_t stop) {
		XPSetWidgetProperty(edit_field, xpProperty_EditFieldSelStart, start);
		XPSetWidgetProperty(edit_field, xpProperty_EditFieldSelEnd, stop);
	}

	void updateChangeButton() {
		switch(change_filter_state) {
			case 0:
				XPSetWidgetDescriptor(change_filter_button, "Ch");
				XPSetWidgetProperty(change_filter_button, xpProperty_ButtonState, 0);
				break;
			case 1:
				XPSetWidgetDescriptor(change_filter_button, "ch");
				XPSetWidgetProperty(change_filter_button, xpProperty_ButtonState, 1);
				break;
			case 2:
				XPSetWidgetDescriptor(change_filter_button, "CH");
				XPSetWidgetProperty(change_filter_button, xpProperty_ButtonState, 1);
				break;
		}
	}

	void updateCrDrFilterButton() {
		switch(cr_dr_filter_state) {
			case 0:
				XPSetWidgetDescriptor(cr_dr_filter_button, "dat");
				XPSetWidgetProperty(cr_dr_filter_button, xpProperty_ButtonState, 1);
				break;
			case 1:
				XPSetWidgetDescriptor(cr_dr_filter_button, "com");
				XPSetWidgetProperty(cr_dr_filter_button, xpProperty_ButtonState, 1);
				break;
			case 2:
				XPSetWidgetDescriptor(cr_dr_filter_button, "d+c");
				XPSetWidgetProperty(cr_dr_filter_button, xpProperty_ButtonState, 0);
				break;
		}
	}

	void doSearch() {
		deselectEditField();

		char searchfield_text[1024];
		XPGetWidgetDescriptor(search_field, searchfield_text, 1024);

        this->refs = ::refs->search(searchfield_text, isRegex(), false == isCaseSensitive(), isChanged(), isOnlyBigChanges(), includeDataRefs(), includeCommandRefs());

		updateScroll();

		std::string window_title = std::string("DataRef Tool (") + std::to_string(refs.size()) + ")"; 
		XPSetWidgetDescriptor(window, window_title.c_str());
	}

	void draw() {
		int left, top, right, bottom;
		XPGetWidgetGeometry(window, &left, &top, &right, &bottom);

		top -= title_bar_height;
		left += mouse_drag_margin;

		const int scroll_pos = (int)XPGetWidgetProperty(scroll_bar, xpProperty_ScrollBarSliderPosition, nullptr);
		const int scroll_pos_max = (int)XPGetWidgetProperty(scroll_bar, xpProperty_ScrollBarMax, nullptr);

		//high scroll_pos is the top of the scroll bar, opposite how we expect
		const int lines_to_render = std::min<int>(displayed_lines, int(refs.size()));
		list_start_index = scroll_pos_max - scroll_pos;
        
        int list_left, list_right;
        XPGetWidgetGeometry(custom_list, &list_left, nullptr, &list_right, nullptr);
        int list_width = list_right - list_left;
        int list_width_in_chars = list_width / fontwidth;

		const std::chrono::system_clock::time_point now = std::chrono::system_clock::now();

		std::string linetext;
		for(int i = 0; i < lines_to_render; i++) {
			const RefRecord * record = refs[i + list_start_index];
			std::array<float, 3> colors;
			int xstart = left;
			int ystart = top - (i + 1) * fontheight;
			float timediff = 0.001f * std::chrono::duration_cast<std::chrono::milliseconds>(now - record->getLastUpdateTime()).count();
			float timediff_fraction = std::min<float>(1.f, timediff / 10.f);
			const DataRefRecord * dr_record = dynamic_cast<const DataRefRecord *>(record);
			if(nullptr != dr_record) {
				const std::string & label = dr_record->getLabelString();
				size_t max_value_chars = std::max<int>(0, list_width_in_chars - 1 - int(label.size()));
				linetext += label;
				linetext += "=";
				linetext += record->getDisplayString(max_value_chars);
				if(record->isBlacklisted()) {
					colors = {{1.f, .3f, .3f}}; //red ish
				} else {
					colors = {{0.2f + timediff_fraction * 0.8f, 1.f, 1.f}};
				}
			} else {
				const CommandRefRecord * pcr = dynamic_cast<const CommandRefRecord *>(record);
				colors = {{1.f - timediff_fraction, 1.f, 0.f}}; //green
				linetext = pcr->getName();
			}

			XPLMDrawString(colors.data(), xstart, ystart, (char *)linetext.c_str(), nullptr, font);
			linetext.clear();
		}
	}

	void resize() {
		int left, top, right, bottom;
		XPGetWidgetGeometry(window, &left, &top, &right, &bottom);
		resize(left, top, right, bottom);
	}

	void resize(int left, int top, int right, int bottom) {
		top -= title_bar_height;
		left += mouse_drag_margin;
		right -= mouse_drag_margin;
		bottom += mouse_drag_margin;

		XPSetWidgetGeometry(regex_toggle_button, left + 0 * toggle_button_width, bottom + bottom_row_height, left + 1 * toggle_button_width, bottom);
		XPSetWidgetGeometry(case_sensitive_button, left + 1 * toggle_button_width, bottom + bottom_row_height, left + 2 * toggle_button_width, bottom);
		XPSetWidgetGeometry(change_filter_button, left + 2 * toggle_button_width, bottom + bottom_row_height, left + 3 * toggle_button_width, bottom);
		XPSetWidgetGeometry(cr_dr_filter_button, left + 3 * toggle_button_width, bottom + bottom_row_height, left + 4 * toggle_button_width, bottom);
		
		XPSetWidgetGeometry(search_field, left + 4 * toggle_button_width, bottom + bottom_row_height, right, bottom);

		XPSetWidgetGeometry(scroll_bar, right - right_col_width, top, right, bottom + bottom_row_height);
		displayed_lines = (top - bottom - bottom_row_height - mouse_drag_margin) / fontheight;

		XPSetWidgetGeometry(custom_list, left, top, right - right_col_width, bottom + bottom_row_height);
		updateScroll();
	}

	void show() {
		XPShowWidget(window);
	}
    
    void setCaseSensitive(bool is_case_sensitive) {
        int i = is_case_sensitive ? 1 : 0;
        XPSetWidgetProperty(case_sensitive_button, xpProperty_ButtonState, i);
    }
    
    bool isCaseSensitive() const {
        intptr_t property = XPGetWidgetProperty(case_sensitive_button, xpProperty_ButtonState, nullptr);
        return property != 0;
    }
    
    void setIsRegex(bool is_regex) {
        int i = is_regex ? 1 : 0;
        XPSetWidgetProperty(regex_toggle_button, xpProperty_ButtonState, i);
    }
    
    bool isRegex() const {
        intptr_t property = XPGetWidgetProperty(regex_toggle_button, xpProperty_ButtonState, nullptr);
        return property != 0;
    }
    
    void setIsChanged(bool is_changed, bool only_big_changes) {
        change_filter_state = only_big_changes ? 2 : (is_changed ? 1 : 0);
        updateChangeButton();
    }
    
    bool isChanged() const {
        return 0 != change_filter_state;
    }
    
    bool isOnlyBigChanges() const {
        return 2 == change_filter_state;
    }

	void setCrDrFilter(bool has_datarefs, bool has_commandrefs) {
		if(has_datarefs && has_commandrefs) {
			cr_dr_filter_state = 2;
		} else if(has_datarefs) {
			cr_dr_filter_state = 0;
		} else {
			cr_dr_filter_state = 1;
		}
		updateCrDrFilterButton();
	}

	bool includeDataRefs() const {
		return cr_dr_filter_state == 0 || cr_dr_filter_state == 2;
	}

	bool includeCommandRefs() const {
		return cr_dr_filter_state == 1 || cr_dr_filter_state == 2;
	}
    
    int getWidth() const {
        int l,r;
        XPGetWidgetGeometry(window, &l, nullptr, &r, nullptr);
        return r - l;
    }
    
    int getHeight() const {
        int t, b;
        XPGetWidgetGeometry(window, nullptr, &t, nullptr, &b);
        return t - b;
    }
    
    //left
    int getX() const {
        int x;
        XPGetWidgetGeometry(window, &x, nullptr, nullptr, nullptr);
        return x;
    }
    
    //bottom, ogl coordinates
    int getY() const {
        int y;
        XPGetWidgetGeometry(window, nullptr, nullptr, nullptr, &y);
        return y;
    }
};
/////////////////
std::set<std::unique_ptr<ViewerWindow>> viewer_windows;

size_t countViewerWindows() { return viewer_windows.size(); }

void closeViewerWindow(const ViewerWindow * window) {
	auto ptr_up_equal = [window](const std::unique_ptr<ViewerWindow> & up) -> bool {
		return up.get() == window;
	};

	std::set<std::unique_ptr<ViewerWindow>>::const_iterator window_ptr_loc = std::find_if(viewer_windows.cbegin(), viewer_windows.cend(), ptr_up_equal);

	if(viewer_windows.cend() != window_ptr_loc) {
		viewer_windows.erase(window_ptr_loc);
	}
}

void closeViewerWindows() {
	viewer_windows.clear();
}

void updateWindowsAsDatarefsAdded() {
    for(const std::unique_ptr<ViewerWindow> & window : viewer_windows) {
        window->doSearch();
    }
}

boost::property_tree::ptree getViewerWindowsDetails() {
    boost::property_tree::ptree windows;
    
    for(const std::unique_ptr<ViewerWindow> & pwindow : viewer_windows) {
        boost::property_tree::ptree window_details;
        window_details.put("window_height", pwindow->getHeight());
        window_details.put("window_width", pwindow->getWidth());
        window_details.put("x", pwindow->getX());
        window_details.put("y", pwindow->getY());
        
        window_details.put("case_sensitive", pwindow->isCaseSensitive());
        window_details.put("regex", pwindow->isRegex());
        window_details.put("changed", pwindow->isChanged());
        window_details.put("big_changes_only", pwindow->isOnlyBigChanges());
        
        window_details.put("search_term", pwindow->getSearchText());
        
        windows.push_back(std::make_pair("", window_details));
    }
    
    return windows;
}

const char * KEY_HAS_DR = "has_datarefs";
const char * KEY_HAS_CR = "has_commandrefs";

void showViewerWindow(const boost::property_tree::ptree & window_details) {
    // now construct as if we didnt
	int width, height;
	XPLMGetScreenSize(&width, &height);
    
    const int expected_window_width = 500;
    const int expected_window_height = 400;
    const int expected_l = width/2 - expected_window_width / 2;
    const int expected_b = height/2 - expected_window_height / 2;
    
    //extract parameters, if present
    const int window_width = window_details.get<int>("window_width", expected_window_width);
    const int window_height = window_details.get<int>("window_height", expected_window_height);
    const int l = window_details.get<int>("x", expected_l);
    const int b = window_details.get<int>("y", expected_b);
    
    bool is_case_sensitive = window_details.get<bool>("case_sensitive", false);
    bool is_regex = window_details.get<bool>("regex", true);
    bool is_changed = window_details.get<bool>("changed", false);
    bool is_big_changes = window_details.get<bool>("big_changes_only", false);
	bool has_datarefs = window_details.get<bool>(KEY_HAS_DR, true);
	bool has_commandrefs = window_details.get<bool>(KEY_HAS_CR, true);

	if(false == has_datarefs && false == has_commandrefs) {
		has_datarefs = has_commandrefs = true;
	}
    
    std::string search_term = window_details.get<std::string>("search_term", "");
    
    //now make the window
    std::unique_ptr<ViewerWindow> viewer_window = std::make_unique<ViewerWindow>(l, b + window_height, l + window_width, b);
    
    viewer_window->setCaseSensitive(is_case_sensitive);
    viewer_window->setIsRegex(is_regex);
    viewer_window->setIsChanged(is_changed, is_big_changes);
    viewer_window->setCrDrFilter(has_datarefs, has_commandrefs);
    viewer_window->setSearchText(search_term);
    
    viewer_windows.insert(std::move(viewer_window));
}

void showViewerWindow(bool show_dr, bool show_cr) {
	boost::property_tree::ptree window_details;
	window_details.put(KEY_HAS_DR, show_dr);
	window_details.put(KEY_HAS_CR, show_cr);
    showViewerWindow(window_details);
}

void showViewerWindow() { 
	showViewerWindow(boost::property_tree::ptree());
}
