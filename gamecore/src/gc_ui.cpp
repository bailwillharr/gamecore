#include "gamecore/gc_ui.h"

#include <RmlUi/Core.h>
#include <RmlUi/Debugger.h>

namespace gc {

class MyRenderInterface : public Rml::RenderInterface {

};

class MySystemInterface : public Rml::SystemInterface {

};

UI::UI() {}

bool UI::testInit()
{
	MyRenderInterface render_interface{};
	MySystemInterface system_interface{};

	Rml::SetRenderInterface(&render_interface);
	Rml::SetSystemInterface(&system_interface);

	Rml::Initialise();	

	Rml::Debugger::Initialise(context);

	Rml::LoadFontFace("my_font_file.otf");

	Rml::Context* context = Rml::CreateContext("main", Rml::Vector2i(640, 480));
	if (!context) {
		Rml::Shutdown();
		return false;
	}

	Rml::ElementDocument* document = context->LoadDocument("my_document.rml");
	if (!document) {
		Rml::Shutdown();
		return false;
	}

	document->Show();

	// shutting down releases all resources owned by RmlUI (elements, documents, contexts, or anything returned as a raw pointer)
	Rml::Shutdown();

	return true;
}

} // namespace gc
