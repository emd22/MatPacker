#include "Baker.hpp"
#include "ImageSource.hpp"
#include "Ktx2.hpp"

#include <wx/dataview.h>
#include <wx/dcbuffer.h>
#include <wx/filename.h>
#include <wx/image.h>
#include <wx/notebook.h>
#include <wx/spinctrl.h>
#include <wx/statline.h>
#include <wx/stdpaths.h>
#include <wx/wx.h>

#include <algorithm>
#include <iterator>
#include <memory>
#include <optional>
#include <set>
#include <string>

#include "ThirdParty/mINI.h"

static const char* spcConfigSettingsPath = "config.ini";


/**
 * @brief Persistent application settings, stored as an INI file next to the executable.
 * A missing file or missing keys leave the defaults untouched
 */
class ConfigSettings
{
public:
	ConfigSettings() = default;

	void Load()
	{
		mINI::INIFile file(GetFilePath());
		mINI::INIStructure ini;

		if (!file.read(ini)) {
			return;
		}

		if (ini.has(spcSectionOutput) && ini[spcSectionOutput].has(spcKeyDefaultOutputDir)) {
			mDefaultOutputDir = ini[spcSectionOutput][spcKeyDefaultOutputDir];
		}

		if (ini.has(spcSectionOutput) && ini[spcSectionOutput].has(spcKeyCreateSubfolder)) {
			mbCreateSubfolder = ini[spcSectionOutput][spcKeyCreateSubfolder] == "1";
		}

		if (ini.has(spcSectionOutput) && ini[spcSectionOutput].has(spcKeyCompression)) {
			try {
				const int value = std::stoi(ini[spcSectionOutput][spcKeyCompression]);
				mCompression = value >= 0 && value <= int(eTextureCompression::Astc4x4) ? eTextureCompression(value)
																						: eTextureCompression::None;
			}
			catch (const std::exception&) {
				// Malformed value: keep the default.
			}
		}

		if (ini.has(spcSectionOutput) && ini[spcSectionOutput].has(spcKeyResolutionDivisor)) {
			try {
				mResolutionDivisor = std::max(1, std::stoi(ini[spcSectionOutput][spcKeyResolutionDivisor]));
			}
			catch (const std::exception&) {
				// Malformed value: keep the default.
			}
		}
	}

	/// Writes the settings, keeping any other keys already in the file. Returns false on failure.
	bool Save() const
	{
		mINI::INIFile file(GetFilePath());
		mINI::INIStructure ini;

		// Start from the existing file so unrelated keys survive; ignore failure (file may not exist yet).
		file.read(ini);
		ini[spcSectionOutput][spcKeyDefaultOutputDir] = mDefaultOutputDir;
		ini[spcSectionOutput][spcKeyResolutionDivisor] = std::to_string(mResolutionDivisor);
		ini[spcSectionOutput][spcKeyCompression] = std::to_string(int(mCompression));
		ini[spcSectionOutput][spcKeyCreateSubfolder] = mbCreateSubfolder ? "1" : "0";

		return file.write(ini, true);
	}

	const std::string& GetDefaultOutputDir() const { return mDefaultOutputDir; }
	void SetDefaultOutputDir(const std::string& dir) { mDefaultOutputDir = dir; }

	eTextureCompression GetCompression() const { return mCompression; }
	void SetCompression(eTextureCompression compression) { mCompression = compression; }

	int GetResolutionDivisor() const { return mResolutionDivisor; }
	void SetResolutionDivisor(int divisor) { mResolutionDivisor = std::max(1, divisor); }

	bool GetCreateSubfolder() const { return mbCreateSubfolder; }
	void SetCreateSubfolder(bool create) { mbCreateSubfolder = create; }

private:
	static std::string GetFilePath()
	{
		const wxFileName executable(wxStandardPaths::Get().GetExecutablePath());
		return wxFileName(executable.GetPath(), spcConfigSettingsPath).GetFullPath().utf8_string();
	}

	static constexpr const char* spcSectionOutput = "Output";
	static constexpr const char* spcKeyDefaultOutputDir = "DefaultDirectory";
	static constexpr const char* spcKeyResolutionDivisor = "ResolutionDivisor";
	static constexpr const char* spcKeyCompression = "Compression";
	static constexpr const char* spcKeyCreateSubfolder = "CreateSubfolder";

	std::string mDefaultOutputDir;
	int mResolutionDivisor = 1;
	eTextureCompression mCompression = eTextureCompression::None;
	bool mbCreateSubfolder = false;
};

enum class eImageRole
{
	Unused,
	Diffuse,
	Normal,
	Roughness,
	Metallic,
	AmbientOcclusion
};

static constexpr const char* spcRoles[] = { "Unused",			"Diffuse / Base colour",
											"Normal map",		"Roughness (ORM.G)",
											"Metallic (ORM.B)", "AO (ORM.R)" };

/**
 * @brief The nested list of inputs: glTF model -> material -> texture. Images added directly (not from a model)
 * sit under one "Loose images" material. Every material is something that can be exported, and each role is held by
 * at most one texture per material.
 */
class ImageTreeModel : public wxDataViewModel
{
public:
	enum class eKind
	{
		Root,
		Model,
		Material,
		Image
	};

	struct Node
	{
		eKind Kind = eKind::Image;
		Node* Parent = nullptr;
		std::vector<std::unique_ptr<Node>> Children;

		wxString Name;
		wxString SizeText;
		wxString Source; // file or folder the entry came from
		eImageRole Role = eImageRole::Unused;
		ImageSource Image; // Image nodes only
	};

	static constexpr unsigned spcColumnRole = 1;

	ImageTreeModel()
	{
		mRoot = std::make_unique<Node>();
		mRoot->Kind = eKind::Root;
	}

	Node* Root() const { return mRoot.get(); }

	static Node* ToNode(const wxDataViewItem& item) { return static_cast<Node*>(item.GetID()); }

	wxDataViewItem ToItem(const Node* node) const
	{
		return node == mRoot.get() ? wxDataViewItem(nullptr) : wxDataViewItem(const_cast<Node*>(node));
	}

	/// Adds a child under `parent` and tells the view about it.
	Node* Add(Node* parent, eKind kind, const wxString& name, const wxString& source = {},
			  const wxString& size_text = {})
	{
		auto node = std::make_unique<Node>();
		node->Kind = kind;
		node->Parent = parent;
		node->Name = name;
		node->Source = source;
		node->SizeText = size_text;

		Node* raw = node.get();
		parent->Children.push_back(std::move(node));
		ItemAdded(ToItem(parent), ToItem(raw));

		return raw;
	}

	/// Removes a node and everything below it.
	void Remove(Node* node)
	{
		Node* parent = node->Parent;
		auto it = std::find_if(parent->Children.begin(), parent->Children.end(),
							   [node](const std::unique_ptr<Node>& child) { return child.get() == node; });

		// Keep the node alive until the view has been told, but out of the child list so the view does not see it.
		std::unique_ptr<Node> owned = std::move(*it);
		parent->Children.erase(it);
		ItemDeleted(ToItem(parent), ToItem(node));
	}

	void Clear()
	{
		mRoot->Children.clear();
		Cleared();
	}

	/// Removes materials and models that have lost all their textures.
	void Prune()
	{
		std::vector<Node*> empty;

		for (const auto& top : mRoot->Children) {
			if (top->Kind == eKind::Model) {
				bool any_left = false;

				for (const auto& material : top->Children) {
					if (material->Children.empty()) {
						empty.push_back(material.get());
					}
					else {
						any_left = true;
					}
				}

				if (!any_left) {
					empty.push_back(top.get()); // the model goes, taking its empty materials with it
				}
			}
			else if (top->Kind == eKind::Material && top->Children.empty()) {
				empty.push_back(top.get());
			}
		}

		std::set<Node*> doomed(empty.begin(), empty.end());

		for (Node* node : empty) {
			if (!doomed.count(node->Parent)) {
				Remove(node);
			}
		}
	}

	Node* FindModel(const wxString& path) const
	{
		for (const auto& top : mRoot->Children) {
			if (top->Kind == eKind::Model && top->Source == path) {
				return top.get();
			}
		}
		return nullptr;
	}

	Node* FindLooseMaterial() const
	{
		for (const auto& top : mRoot->Children) {
			if (top->Kind == eKind::Material) {
				return top.get();
			}
		}
		return nullptr;
	}

	static Node* FindRole(const Node* material, eImageRole role, const Node* except = nullptr)
	{
		for (const auto& image : material->Children) {
			if (image.get() != except && image->Role == role) {
				return image.get();
			}
		}
		return nullptr;
	}

	/// Every exportable material, in display order.
	void CollectMaterials(std::vector<Node*>& out) const
	{
		for (const auto& top : mRoot->Children) {
			if (top->Kind == eKind::Material) {
				out.push_back(top.get());
			}
			else if (top->Kind == eKind::Model) {
				for (const auto& material : top->Children) {
					out.push_back(material.get());
				}
			}
		}
	}

	// wxDataViewModel

	unsigned GetColumnCount() const override { return 4; }

	wxString GetColumnType(unsigned) const override { return "string"; }

	void GetValue(wxVariant& variant, const wxDataViewItem& item, unsigned column) const override
	{
		const Node* node = ToNode(item);

		if (!node) {
			return;
		}

		switch (column) {
		case 0:
			variant = node->Name;
			break;
		case spcColumnRole:
			variant = wxString(node->Kind == eKind::Image ? spcRoles[int(node->Role)] : "");
			break;
		case 2:
			variant = node->SizeText;
			break;
		case 3:
			variant = node->Source;
			break;
		}
	}

	bool SetValue(const wxVariant& variant, const wxDataViewItem& item, unsigned column) override
	{
		Node* node = ToNode(item);

		if (column != spcColumnRole || !node || node->Kind != eKind::Image) {
			return false;
		}

		const wxString text = variant.GetString();

		for (int i = 0; i < int(std::size(spcRoles)); ++i) {
			if (text == spcRoles[i]) {
				SetRole(node, eImageRole(i));
				return true;
			}
		}

		return false;
	}

	/// A role is held by one texture per material, so giving it to one frees it on the other.
	void SetRole(Node* node, eImageRole role)
	{
		if (role != eImageRole::Unused) {
			while (Node* other = FindRole(node->Parent, role, node)) {
				other->Role = eImageRole::Unused;
				ItemChanged(ToItem(other));
			}
		}

		node->Role = role;
	}

	wxDataViewItem GetParent(const wxDataViewItem& item) const override
	{
		const Node* node = ToNode(item);
		return node && node->Parent ? ToItem(node->Parent) : wxDataViewItem(nullptr);
	}

	bool IsContainer(const wxDataViewItem& item) const override
	{
		return !item.IsOk() || ToNode(item)->Kind != eKind::Image;
	}

	bool HasContainerColumns(const wxDataViewItem&) const override { return true; }

	bool IsEnabled(const wxDataViewItem& item, unsigned column) const override
	{
		// Only textures have a role to choose.
		return column != spcColumnRole || (item.IsOk() && ToNode(item)->Kind == eKind::Image);
	}

	unsigned GetChildren(const wxDataViewItem& item, wxDataViewItemArray& children) const override
	{
		const Node* node = item.IsOk() ? ToNode(item) : mRoot.get();

		for (const auto& child : node->Children) {
			children.Add(ToItem(child.get()));
		}

		return unsigned(node->Children.size());
	}

private:
	std::unique_ptr<Node> mRoot;
};

// Shows one image scaled to fit, using nearest-neighbour when enlarging so
// small mips look blocky.
class ImageCanvas : public wxPanel
{
public:
	explicit ImageCanvas(wxWindow* parent) : wxPanel(parent)
	{
		SetBackgroundStyle(wxBG_STYLE_PAINT);
		SetMinSize(wxSize(320, 240));
		Bind(wxEVT_PAINT, &ImageCanvas::OnPaint, this);
		Bind(wxEVT_SIZE,
			 [this](wxSizeEvent& e)
			 {
				 mbDirty = true;
				 Refresh();
				 e.Skip();
			 });
	}

	void SetImage(const MPImage* img)
	{
		mImage = wxImage();

		if (img && !img->IsEmpty()) {
			const size_t n = static_cast<size_t>(img->Width) * img->Height;
			auto* rgb = static_cast<unsigned char*>(malloc(n * 3));
			auto* alpha = static_cast<unsigned char*>(malloc(n));

			for (size_t i = 0; i < n; ++i) {
				rgb[i * 3] = img->Pixels[i * 4];
				rgb[i * 3 + 1] = img->Pixels[i * 4 + 1];
				rgb[i * 3 + 2] = img->Pixels[i * 4 + 2];
				alpha[i] = img->Pixels[i * 4 + 3];
			}

			mImage = wxImage(img->Width, img->Height, rgb, alpha, false);
		}

		mbDirty = true;
		Refresh();
	}

private:
	void OnPaint(wxPaintEvent&)
	{
		wxAutoBufferedPaintDC dc(this);

		dc.SetBackground(wxBrush(wxColour(48, 48, 48)));
		dc.Clear();

		if (!mImage.IsOk()) {
			return;
		}

		if (mbDirty) {
			const wxSize cs = GetClientSize();
			const double scale = std::min(static_cast<double>(cs.x) / mImage.GetWidth(),
										  static_cast<double>(cs.y) / mImage.GetHeight());

			const int w = std::max(1, static_cast<int>(mImage.GetWidth() * scale));
			const int h = std::max(1, static_cast<int>(mImage.GetHeight() * scale));

			mBitmap = wxBitmap(mImage.Scale(w, h, scale >= 1.0 ? wxIMAGE_QUALITY_NEAREST : wxIMAGE_QUALITY_HIGH));
			mbDirty = false;
		}

		const wxSize cs = GetClientSize();
		dc.DrawBitmap(mBitmap, (cs.x - mBitmap.GetWidth()) / 2, (cs.y - mBitmap.GetHeight()) / 2, true);
	}

private:
	wxImage mImage;
	wxBitmap mBitmap;
	bool mbDirty = true;
};

class MainFrame : public wxFrame
{
public:
	MainFrame() : wxFrame(nullptr, wxID_ANY, "MeatPacker", wxDefaultPosition, wxSize(850, 1000))
	{
		mConfig.Load();

		auto* panel = new wxPanel(this);
		auto* root = new wxBoxSizer(wxVERTICAL);

		const wxString image_wildcard = "Images (*.png;*.jpg;*.jpeg;*.tga;*.bmp;*.psd;*.gif;*.hdr;*.pic;*.pnm)|"
										"*.png;*.jpg;*.jpeg;*.tga;*.bmp;*.psd;*.gif;*.hdr;*.pic;*.pnm|All "
										"files|*.*";

		auto* inputs = new wxStaticBoxSizer(wxVERTICAL, panel, "Input Images");
		wxWindow* ip = inputs->GetStaticBox();

		wxBoxSizer* buttons = new wxBoxSizer(wxHORIZONTAL);
		wxButton* add_btn = new wxButton(ip, wxID_ANY, "Add...");
		wxButton* remove_btn = new wxButton(ip, wxID_ANY, "Remove Selected");
		wxButton* clear_btn = new wxButton(ip, wxID_ANY, "Clear");

		buttons->Add(add_btn, 0, wxRIGHT, 6);
		buttons->Add(remove_btn, 0, wxRIGHT, 6);
		buttons->Add(clear_btn, 0);
		inputs->Add(buttons, 0, wxALL, 6);

		mpTree = new wxDataViewCtrl(ip, wxID_ANY, wxDefaultPosition, wxSize(-1, 230), wxDV_MULTIPLE | wxDV_ROW_LINES);
		mpModel = new ImageTreeModel();
		mpTree->AssociateModel(mpModel);
		mpModel->DecRef(); // the control now owns it

		wxArrayString role_choices;
		for (const char* r : spcRoles) {
			role_choices.Add(r);
		}

		wxDataViewColumn* name_column = mpTree->AppendTextColumn("Name", 0, wxDATAVIEW_CELL_INERT, 260);
		mpTree->AppendColumn(new wxDataViewColumn("Role",
												  new wxDataViewChoiceRenderer(role_choices, wxDATAVIEW_CELL_EDITABLE),
												  ImageTreeModel::spcColumnRole, 170));
		mpTree->AppendTextColumn("Size", 2, wxDATAVIEW_CELL_INERT, 90);
		mpTree->AppendTextColumn("Source", 3, wxDATAVIEW_CELL_INERT, 300);
		mpTree->SetExpanderColumn(name_column);

		inputs->Add(mpTree, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 6);
		root->Add(inputs, 0, wxEXPAND | wxALL, 8);

		add_btn->Bind(wxEVT_BUTTON, &MainFrame::OnAddImages, this);
		remove_btn->Bind(wxEVT_BUTTON, &MainFrame::OnRemoveSelected, this);
		clear_btn->Bind(wxEVT_BUTTON,
						[this](wxCommandEvent&)
						{
							mpModel->Clear();
							RefreshMaterialList(nullptr);
						});
		mpTree->Bind(wxEVT_DATAVIEW_SELECTION_CHANGED, &MainFrame::OnTreeSelection, this);

		auto* defs = new wxStaticBoxSizer(wxVERTICAL, panel, "Fallback values");
		auto* dgrid = new wxFlexGridSizer(2, 6, 8);

		auto AddSpinner = [&](const wxString& label, double init, wxSpinCtrlDouble*& out)
		{
			dgrid->Add(new wxStaticText(defs->GetStaticBox(), wxID_ANY, label), 0, wxALIGN_CENTER_VERTICAL);
			out = new wxSpinCtrlDouble(defs->GetStaticBox(), wxID_ANY, "", wxDefaultPosition, wxDefaultSize,
									   wxSP_ARROW_KEYS, 0.0, 1.0, init, 0.05);
			out->SetDigits(3);
			dgrid->Add(out);
		};

		AddSpinner("Roughness", 0.5, mpDefRoughness);
		AddSpinner("Metallic", 0.0, mpDefMetallic);
		AddSpinner("AO", 1.0, mpDefAmbientOcclusion);

		defs->Add(dgrid, 0, wxALL, 6);

		auto* material_box = new wxStaticBoxSizer(wxVERTICAL, panel, "Material to export");
		mpMaterialList = new wxListBox(material_box->GetStaticBox(), wxID_ANY, wxDefaultPosition, wxSize(-1, 60), 0,
									   nullptr, wxLB_SINGLE);
		material_box->Add(mpMaterialList, 1, wxEXPAND | wxALL, 6);
		mpMaterialList->Bind(wxEVT_LISTBOX, &MainFrame::OnMaterialSelected, this);

		auto* defs_row = new wxBoxSizer(wxHORIZONTAL);
		defs_row->Add(defs, 0, wxEXPAND | wxRIGHT, 8);
		defs_row->Add(material_box, 1, wxEXPAND);
		root->Add(defs_row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);

		auto* out = new wxStaticBoxSizer(wxVERTICAL, panel, "Output");
		auto* ogrid = new wxFlexGridSizer(2, 6, 8);
		ogrid->AddGrowableCol(1);
		ogrid->Add(new wxStaticText(out->GetStaticBox(), wxID_ANY, "Directory"), 0, wxALIGN_CENTER_VERTICAL);
		auto* dir_row = new wxBoxSizer(wxHORIZONTAL);
		mpOutDir = new wxTextCtrl(out->GetStaticBox(), wxID_ANY, wxString::FromUTF8(mConfig.GetDefaultOutputDir()));
		auto* dir_browse = new wxButton(out->GetStaticBox(), wxID_ANY, "Browse");
		auto* dir_set_default = new wxButton(out->GetStaticBox(), wxID_ANY, "Set as default");

		dir_browse->Bind(wxEVT_BUTTON,
						 [this](wxCommandEvent&)
						 {
							 wxDirDialog dlg(this, "Select output directory", mpOutDir->GetValue(),
											 wxDD_DEFAULT_STYLE | wxDD_NEW_DIR_BUTTON);
							 if (dlg.ShowModal() == wxID_OK)
								 mpOutDir->SetValue(dlg.GetPath());
						 });

		dir_set_default->Bind(wxEVT_BUTTON,
							  [this](wxCommandEvent&)
							  {
								  mConfig.SetDefaultOutputDir(mpOutDir->GetValue().utf8_string());

								  if (mConfig.Save()) {
									  mpLog->AppendText("Default output directory saved.\n");
								  }
								  else {
									  mpLog->AppendText("Error: could not save the default output directory.\n");
								  }
							  });

		dir_row->Add(mpOutDir, 1, wxEXPAND | wxRIGHT, 6);
		dir_row->Add(dir_browse, 0, wxRIGHT, 6);
		dir_row->Add(dir_set_default, 0);

		ogrid->Add(dir_row, 1, wxEXPAND);
		ogrid->Add(new wxStaticText(out->GetStaticBox(), wxID_ANY, "Base Name"), 0, wxALIGN_CENTER_VERTICAL);

		mpName = new wxTextCtrl(out->GetStaticBox(), wxID_ANY, "material");
		ogrid->Add(mpName, 1, wxEXPAND);

		ogrid->Add(new wxStaticText(out->GetStaticBox(), wxID_ANY, "Resolution"), 0, wxALIGN_CENTER_VERTICAL);

		mpDivisor = new wxChoice(out->GetStaticBox(), wxID_ANY);
		for (const char* label : spcDivisorLabels) {
			mpDivisor->Append(label);
		}

		// Restore the saved divisor; fall back to full size if it is not one of the offered values.
		int divisor_index = 0;
		for (int i = 0; i < int(std::size(spcDivisors)); ++i) {
			if (spcDivisors[i] == mConfig.GetResolutionDivisor()) {
				divisor_index = i;
			}
		}

		mpDivisor->SetSelection(divisor_index);

		mpDivisor->Bind(wxEVT_CHOICE,
						[this](wxCommandEvent&)
						{
							mConfig.SetResolutionDivisor(spcDivisors[std::max(0, mpDivisor->GetSelection())]);

							if (!mConfig.Save()) {
								mpLog->AppendText("Error: could not save the resolution setting.\n");
							}
						});

		ogrid->Add(mpDivisor, 0);

		ogrid->Add(new wxStaticText(out->GetStaticBox(), wxID_ANY, "Compression"), 0, wxALIGN_CENTER_VERTICAL);

		mpCompression = new wxChoice(out->GetStaticBox(), wxID_ANY);
		for (const char* label : spcCompressionLabels) {
			mpCompression->Append(label);
		}

		mpCompression->SetSelection(int(mConfig.GetCompression()));
		mpCompression->Bind(wxEVT_CHOICE,
							[this](wxCommandEvent&)
							{
								mConfig.SetCompression(eTextureCompression(std::max(0, mpCompression->GetSelection())));

								if (!mConfig.Save()) {
									mpLog->AppendText("Error: could not save the compression setting.\n");
								}
							});

		ogrid->Add(mpCompression, 0);
		out->Add(ogrid, 0, wxEXPAND | wxALL, 6);

		mpMips = new wxCheckBox(out->GetStaticBox(), wxID_ANY, "Generate Mipmaps");
		mpMips->SetValue(true);

		out->Add(mpMips, 0, wxLEFT | wxBOTTOM, 8);

		mpSubfolder = new wxCheckBox(out->GetStaticBox(), wxID_ANY, "Create Subfolder");

		mpSubfolder->SetValue(mConfig.GetCreateSubfolder());
		mpSubfolder->Bind(wxEVT_CHECKBOX,
						  [this](wxCommandEvent&)
						  {
							  mConfig.SetCreateSubfolder(mpSubfolder->GetValue());

							  if (!mConfig.Save()) {
								  mpLog->AppendText("Error: could not save the subfolder setting.\n");
							  }
						  });

		out->Add(mpSubfolder, 0, wxLEFT | wxBOTTOM, 8);
		root->Add(out, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);

		auto* bake_all_button = new wxButton(panel, wxID_ANY, "Bake All");
		root->Add(bake_all_button, 1, wxALIGN_RIGHT | wxLEFT | wxRIGHT | wxBOTTOM, 8);

		auto* bake = new wxButton(panel, wxID_ANY, "Single Bake");
		root->Add(bake, 1, wxALIGN_RIGHT | wxLEFT | wxRIGHT | wxBOTTOM, 8);


		mpTabs = new wxNotebook(panel, wxID_ANY);
		mpLog = new wxTextCtrl(mpTabs, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxTE_MULTILINE | wxTE_READONLY);
		mpTabs->AddPage(mpLog, "Log");

		auto* viewer = new wxPanel(mpTabs);
		auto* vsizer = new wxBoxSizer(wxHORIZONTAL);
		auto* left = new wxBoxSizer(wxVERTICAL);

		mpTexChoice = new wxChoice(viewer, wxID_ANY);
		mpLevelList = new wxListBox(viewer, wxID_ANY, wxDefaultPosition, wxSize(200, -1));
		mpLevelInfo = new wxStaticText(viewer, wxID_ANY, "Bake to view the mip levels.");

		left->Add(mpTexChoice, 0, wxEXPAND | wxBOTTOM, 6);
		left->Add(mpLevelList, 1, wxEXPAND | wxBOTTOM, 6);
		left->Add(mpLevelInfo, 0, wxEXPAND);

		mpCanvas = new ImageCanvas(viewer);
		vsizer->Add(left, 0, wxEXPAND | wxALL, 6);
		vsizer->Add(mpCanvas, 1, wxEXPAND | wxALL, 6);
		viewer->SetSizer(vsizer);
		mpTabs->AddPage(viewer, "Mip Viewer", true);

		root->Add(mpTabs, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);

		mpTexChoice->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { ShowTexture(mpTexChoice->GetSelection()); });
		mpLevelList->Bind(wxEVT_LISTBOX, [this](wxCommandEvent&) { ShowLevel(mpLevelList->GetSelection()); });

		panel->SetSizer(root);

		bake->Bind(wxEVT_BUTTON, &MainFrame::OnSingleBake, this);
		bake_all_button->Bind(wxEVT_BUTTON, &MainFrame::OnBakeAll, this);
	}

private:
	// Output resolution divisors offered in the UI; labels and values must stay in the same order.
	// In the same order as eTextureCompression.
	static constexpr const char* spcCompressionLabels[] = { "None (raw RGBA8)", "Zstd (lossless, smaller file)",
															"Basis UASTC + Zstd (BC7 / ASTC on load)",
															"ASTC 4x4 (mobile / Apple GPUs)" };

	static constexpr int spcDivisors[] = { 1, 2, 4, 8 };
	static constexpr const char* spcDivisorLabels[] = { "Full size", "1/2 (half)", "1/4 (quarter)", "1/8 (eighth)" };

	// Reads just the image header, so this is cheap even for large files.
	static wxString GetImageSizeText(const ImageSource& source)
	{
		int width = 0, height = 0;

		if (!GetImageSize(source, width, height)) {
			return "unreadable";
		}

		return wxString::Format("%d x %d", width, height);
	}

	// Guesses a role from common texture naming ("rock_Normal.png", "wall_ao.jpg").
	static eImageRole GuessRole(const wxString& filename)
	{
		wxString stem = wxFileName(filename).GetName().Lower();
		wxString spaced = stem;

		for (size_t i = 0; i < spaced.length(); ++i) {
			if (!wxIsalnum(spaced[i])) {
				spaced[i] = ' ';
			}
		}

		wxArrayString tokens = wxSplit(spaced, ' ');

		auto FileNameContains = [&](std::initializer_list<const char*> words)
		{
			for (const char* w : words) {
				if (stem.Contains(w) || tokens.Index(w) != wxNOT_FOUND) {
					return true;
				}
			}

			return false;
		};

		if (FileNameContains({ "normal", "nrm", "nor" })) {
			return eImageRole::Normal;
		}
		if (FileNameContains({ "rough", "rgh" })) {
			return eImageRole::Roughness;
		}
		if (FileNameContains({ "metal", "mtl" })) {
			return eImageRole::Metallic;
		}
		if (tokens.Index("ao") != wxNOT_FOUND || FileNameContains({ "occlusion", "ambient" })) {
			return eImageRole::AmbientOcclusion;
		}
		if (FileNameContains({ "diffuse", "albedo", "basecolor", "base", "color", "diff", "col" })) {
			return eImageRole::Diffuse;
		}

		return eImageRole::Unused;
	}

	/**
	 * @brief Find a common base name to generate a default value for the material prefix. Returns nothin if there is no
	 * common name.
	 */
	std::optional<wxString> FindCommonBaseName(const wxArrayString& paths) const
	{
		std::optional<wxString> common;

		for (const wxString& path : paths) {
			const wxString stem = wxFileName(path).GetName();

			// Find last underscore
			const int split = stem.Find('_', true);

			if (split <= 0) {
				continue;
			}

			const wxString prefix = stem.Left(split);

			if (prefix.Length() > 1) {
				return std::make_optional<wxString>(prefix);
			}
		}

		return std::nullopt;
	}

	using Node = ImageTreeModel::Node;

	// The material picked in the "Material to export" list, if any.
	Node* SelectedMaterial() const
	{
		const int index = mpMaterialList->GetSelection();
		return index >= 0 && index < static_cast<int>(mMaterials.size()) ? mMaterials[index] : nullptr;
	}

	static wxString MaterialLabel(const Node* material)
	{
		if (material->Parent->Kind == ImageTreeModel::eKind::Model) {
			return material->Parent->Name + " / " + material->Name;
		}

		return material->Name;
	}

	/// Rebuilds the material list. Keeps `preferred` selected if it exists, otherwise the material that was selected
	/// before, otherwise the only material if there is just one.
	void RefreshMaterialList(const Node* preferred)
	{
		const Node* previous = SelectedMaterial();

		mMaterials.clear();
		mpModel->CollectMaterials(mMaterials);
		mpMaterialList->Clear();

		int selection = mMaterials.size() == 1 ? 0 : -1;

		for (int i = 0; i < int(mMaterials.size()); ++i) {
			mpMaterialList->Append(MaterialLabel(mMaterials[i]));

			if (mMaterials[i] == preferred || (!preferred && mMaterials[i] == previous)) {
				selection = i;
			}
		}

		if (selection >= 0) {
			mpMaterialList->SetSelection(selection);
			UpdateBaseName(mMaterials[selection]);
		}
	}

	/// A glTF material names the set; loose images fall back to guessing from their file names.
	void UpdateBaseName(const Node* material)
	{
		if (material->Parent->Kind == ImageTreeModel::eKind::Model) {
			wxString name = material->Name;

			for (const char* bad : { "/", "\\", ":" }) {
				name.Replace(bad, "_");
			}

			mpName->SetValue(name);
			return;
		}

		wxArrayString image_paths;
		for (const auto& image : material->Children) {
			image_paths.Add(wxString::FromUTF8(image->Image.Path));
		}

		auto base_name = FindCommonBaseName(image_paths);

		if (base_name.has_value()) {
			mpName->SetValue(base_name.value());
		}
	}

	void ExpandNode(const Node* node) { mpTree->Expand(mpModel->ToItem(node)); }

	void AddLooseImage(const wxString& path)
	{
		ImageSource source;
		source.Path = path.utf8_string();

		Node* loose = mpModel->FindLooseMaterial();

		if (!loose) {
			loose = mpModel->Add(mpModel->Root(), ImageTreeModel::eKind::Material, "Loose images");
		}

		for (const auto& image : loose->Children) {
			if (image->Image == source) {
				return;
			}
		}

		// Only auto-assign a guessed role if nothing in this material already has it
		eImageRole role = GuessRole(path);
		if (role != eImageRole::Unused && ImageTreeModel::FindRole(loose, role)) {
			role = eImageRole::Unused;
		}

		Node* node = mpModel->Add(loose, ImageTreeModel::eKind::Image, wxFileName(path).GetFullName(),
								  wxFileName(path).GetPath(), GetImageSizeText(source));
		node->Image = source;
		node->Role = role;

		ExpandNode(loose);
	}

	/**
	 * @brief Loads every material of a .gltf/.glb, each with its textures nested beneath it and their roles assigned
	 * from the material's texture slots.
	 * @return The first material added, if any.
	 */
	Node* AddGltfModel(const wxString& path)
	{
		if (mpModel->FindModel(path)) {
			return nullptr;
		}

		std::vector<GltfMaterial> materials;
		std::string error;

		if (!ListGltfMaterials(path.utf8_string(), materials, error)) {
			wxMessageBox(wxString::FromUTF8(error), "glTF error", wxOK | wxICON_ERROR, this);
			return nullptr;
		}

		Node* model = mpModel->Add(mpModel->Root(), ImageTreeModel::eKind::Model, wxFileName(path).GetFullName(), path);
		Node* first_material = nullptr;

		struct SlotRole
		{
			const GltfTextureSlot& Slot;
			eImageRole Role;
			const char* Label;
		};

		for (const GltfMaterial& material : materials) {
			const SlotRole slots[] = {
				{ material.BaseColor, eImageRole::Diffuse, "Base Colour" },
				{ material.Normal, eImageRole::Normal, "Normal Map" },
				{ material.Occlusion, eImageRole::AmbientOcclusion, "AO (R)" },
				{ material.Roughness, eImageRole::Roughness, "Roughness (G)" },
				{ material.Metallic, eImageRole::Metallic, "Metallic (B)" },
			};

			Node* material_node = nullptr;

			for (const SlotRole& entry : slots) {
				if (entry.Slot.Empty()) {
					continue;
				}

				if (!material_node) {
					material_node = mpModel->Add(model, ImageTreeModel::eKind::Material,
												 wxString::FromUTF8(material.Name), path);
				}

				const wxString size_text = entry.Slot.Width > 0
											   ? wxString::Format("%d x %d", entry.Slot.Width, entry.Slot.Height)
											   : wxString("unreadable");

				Node* node = mpModel->Add(material_node, ImageTreeModel::eKind::Image, entry.Label, path, size_text);
				node->Image = entry.Slot.Source;
				node->Role = entry.Role;
			}

			if (material_node) {
				ExpandNode(material_node);
				first_material = first_material ? first_material : material_node;
			}
		}

		if (!first_material) {
			mpModel->Remove(model);
			wxMessageBox(wxFileName(path).GetFullName() + " has no materials with textures.", "glTF",
						 wxOK | wxICON_INFORMATION, this);
			return nullptr;
		}

		ExpandNode(model);
		return first_material;
	}

	/**
	 * @brief Adds images to the input tree and auto-assigns their roles. A .gltf/.glb adds all of its materials,
	 * each nested under the model.
	 */
	void OnAddImages(wxCommandEvent&)
	{
		const wxString wildcard = "Images and glTF models (*.png;*.jpg;*.jpeg;*.tga;*.bmp;*.psd;*.gif;*.hdr;*.pic;*."
								  "pnm;*.gltf;*.glb)|*.png;*.jpg;*.jpeg;*.tga;*.bmp;*.psd;*.gif;*.hdr;*.pic;*.pnm;*."
								  "gltf;*.glb|All files|*.*";

		wxFileDialog dialog(this, "Select images or a glTF model", mLastDir, "", wildcard,
							wxFD_OPEN | wxFD_MULTIPLE | wxFD_FILE_MUST_EXIST);

		if (dialog.ShowModal() != wxID_OK) {
			return;
		}

		wxArrayString paths;
		dialog.GetPaths(paths);

		const Node* to_select = nullptr;
		bool added_loose = false;

		for (const wxString& path : paths) {
			mLastDir = wxFileName(path).GetPath();

			if (IsGltfPath(path.utf8_string())) {
				if (const Node* material = AddGltfModel(path)) {
					to_select = to_select ? to_select : material;
				}
			}
			else {
				AddLooseImage(path);
				added_loose = true;
			}
		}

		// Prefer the newly loaded model's first material; otherwise the loose images.
		if (!to_select && added_loose) {
			to_select = mpModel->FindLooseMaterial();
		}

		RefreshMaterialList(to_select);
	}

	void OnRemoveSelected(wxCommandEvent&)
	{
		wxDataViewItemArray items;
		mpTree->GetSelections(items);

		std::set<Node*> selected;
		for (const wxDataViewItem& item : items) {
			selected.insert(ImageTreeModel::ToNode(item));
		}

		// Anything under a selected node goes with it.
		std::vector<Node*> to_remove;
		for (Node* node : selected) {
			bool ancestor_selected = false;

			for (Node* a = node->Parent; a; a = a->Parent) {
				ancestor_selected = ancestor_selected || selected.count(a) > 0;
			}

			if (!ancestor_selected) {
				to_remove.push_back(node);
			}
		}

		const Node* previous = SelectedMaterial();

		for (Node* node : to_remove) {
			mpModel->Remove(node);
		}

		mpModel->Prune();
		RefreshMaterialList(previous);
	}

	// Choosing a material to export also shows it in the tree and names the output after it.
	void OnMaterialSelected(wxCommandEvent&)
	{
		if (const Node* material = SelectedMaterial()) {
			const wxDataViewItem item = mpModel->ToItem(material);

			mpTree->UnselectAll();
			mpTree->Select(item);
			mpTree->EnsureVisible(item);
			UpdateBaseName(material);
		}
	}

	// Selecting anything in the tree picks the material it belongs to.
	void OnTreeSelection(wxDataViewEvent& event)
	{
		Node* node = event.GetItem().IsOk() ? ImageTreeModel::ToNode(event.GetItem()) : nullptr;

		while (node && node->Kind != ImageTreeModel::eKind::Material) {
			node = node->Kind == ImageTreeModel::eKind::Image ? node->Parent : nullptr;
		}

		if (!node) {
			return;
		}

		const auto it = std::find(mMaterials.begin(), mMaterials.end(), node);

		if (it != mMaterials.end() && node != SelectedMaterial()) {
			mpMaterialList->SetSelection(int(it - mMaterials.begin()));
			UpdateBaseName(node);
		}
	}

	void DoMaterialBake(Node* material)
	{
		BakeSettings bake_settings;

		if (material == nullptr) {
			mpLog->Clear();
			mpLog->AppendText("Error: select a material to export.\n");
			mpTabs->SetSelection(0);
			return;
		}

		for (const auto& image : material->Children) {
			switch (image->Role) {
			case eImageRole::Diffuse:
				bake_settings.PathDiffuse = image->Image;
				break;
			case eImageRole::Normal:
				bake_settings.PathNormalMap = image->Image;
				break;
			case eImageRole::Roughness:
				bake_settings.PathRoughness = image->Image;
				break;
			case eImageRole::Metallic:
				bake_settings.PathMetallic = image->Image;
				break;
			case eImageRole::AmbientOcclusion:
				bake_settings.PathAO = image->Image;
				break;
			case eImageRole::Unused:
				break;
			}
		}

		bake_settings.DefaultRoughness = float(mpDefRoughness->GetValue());
		bake_settings.DefaultMetallic = float(mpDefMetallic->GetValue());
		bake_settings.DefaultAO = float(mpDefAmbientOcclusion->GetValue());
		bake_settings.OutputDir = mpOutDir->GetValue().ToStdString();
		bake_settings.BaseName = mpName->GetValue().ToStdString();
		bake_settings.bExportMipmaps = mpMips->GetValue();
		bake_settings.bCreateSubfolder = mpSubfolder->GetValue();
		bake_settings.ResolutionDivisor = spcDivisors[std::max(0, mpDivisor->GetSelection())];
		bake_settings.Compression = eTextureCompression(std::max(0, mpCompression->GetSelection()));

		mpLog->Clear();
		wxBusyCursor busy;
		std::vector<std::string> written;

		bool ok = Bake(
			bake_settings,
			[this](const std::string& line)
			{
				mpLog->AppendText(wxString::FromUTF8(line) + "\n");
				wxYield();
			},
			&written);

		mpLog->AppendText(ok ? "Done.\n" : "Finished with errors.\n");

		LoadViewer(written);

		if (ok && !written.empty()) {
			mpTabs->SetSelection(1);
		}
	}


	/**
	 * @brief Bake all materials in the materials list
	 */
	void OnBakeAll(wxCommandEvent&)
	{
		for (Node* node : mMaterials) {
			if (node == nullptr) {
				continue;
			}

			UpdateBaseName(node);
			DoMaterialBake(node);
		}
	}

	void OnSingleBake(wxCommandEvent&) { DoMaterialBake(SelectedMaterial()); }

	// Reads the baked files back so the viewer shows exactly what is on disk.
	void LoadViewer(const std::vector<std::string>& files)
	{
		mTextures.clear();
		mpTexChoice->Clear();

		for (const std::string& path : files) {
			std::vector<MPImage> levels;
			std::string e = ReadKTX2Levels(path, levels);

			if (!e.empty()) {
				mpLog->AppendText("Viewer: " + wxString::FromUTF8(e) + "\n");
				continue;
			}

			mTextures.push_back(std::move(levels));
			mpTexChoice->Append(wxFileName(wxString::FromUTF8(path)).GetFullName());
		}

		if (mTextures.empty()) {
			mpLevelList->Clear();
			mpCanvas->SetImage(nullptr);
			mpLevelInfo->SetLabel("No textures to show.");
			return;
		}

		mpTexChoice->SetSelection(0);
		ShowTexture(0);
	}

	void ShowTexture(int index)
	{
		mpLevelList->Clear();

		if (index < 0 || index >= int(mTextures.size())) {
			return;
		}

		const auto& levels = mTextures[index];

		for (size_t i = 0; i < levels.size(); i++) {
			mpLevelList->Append(wxString::Format("Level %zu  -  %d x %d", i, levels[i].Width, levels[i].Height));
		}

		mpLevelList->SetSelection(0);
		ShowLevel(0);
	}

	void ShowLevel(int level)
	{
		const int tex = mpTexChoice->GetSelection();

		if (tex < 0 || level < 0 || level >= int(mTextures[tex].size())) {
			return;
		}

		const MPImage& img = mTextures[tex][level];
		mpCanvas->SetImage(&img);
		mpLevelInfo->SetLabel(wxString::Format("%d x %d  (%zu bytes)", img.Width, img.Height, img.Pixels.size()));
	}

private:
	wxDataViewCtrl* mpTree;
	ImageTreeModel* mpModel; // owned by mpTree
	wxListBox* mpMaterialList;
	std::vector<Node*> mMaterials; // parallel to the entries of mpMaterialList
	wxString mLastDir;
	wxSpinCtrlDouble *mpDefRoughness, *mpDefMetallic, *mpDefAmbientOcclusion;
	wxTextCtrl *mpOutDir, *mpName, *mpLog;
	wxCheckBox* mpMips;
	wxCheckBox* mpSubfolder;
	wxChoice* mpDivisor;
	wxChoice* mpCompression;
	wxNotebook* mpTabs;
	wxChoice* mpTexChoice;
	wxListBox* mpLevelList;
	wxStaticText* mpLevelInfo;
	ImageCanvas* mpCanvas;

	std::vector<std::vector<MPImage>> mTextures;
	ConfigSettings mConfig;
};

class App : public wxApp
{
public:
	bool OnInit() override
	{
		SetAppearance(Appearance::Dark);
		(new MainFrame)->Show();
		return true;
	}
};

wxIMPLEMENT_APP(App);
