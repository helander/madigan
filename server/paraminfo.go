package main
// #cgo pkg-config: lilv-0
// #include <lilv/lilv.h>
// #include <stdlib.h>
import "C"
import (
        "encoding/json"
        "fmt"
        "net/http"
	"unsafe"
)

type Point struct {
        Label       string     `json:"label"`
        Value       float32    `json:"value"`
}

type Info struct {
	Index   string  `json:"index"`
	Symbol  string  `json:"symbol"`
	Name    string  `json:"name"`
	Input   bool    `json:"input,omitempty"`
	Output  bool    `json:"output,omitempty"`
	Audio   bool    `json:"audio,omitempty"`
	Control bool    `json:"control,omitempty"`
	Atom    bool    `json:"atom,omitempty"`
	Default float32 `json:"default,omitempty"`
	Min     float32 `json:"min,omitempty"`
	Max     float32 `json:"max,omitempty"`
	Prio    float32 `json:"prio"`
	Scale   []Point `json:"scale,omitempty"`

	Enum    bool    `json:"enum"`
	Toggle  bool    `json:"toggle"`
	Uri     string  `json:"uri"`
	Midicc  string  `json:"midicc,omitempty"`

	Range  string `json:"range"`
}

type AllInfo struct {
       ControlInput []Info   `json:"control"`
       MidiParameter []Info   `json:"midi"`
       PatchParameter []Info  `json:"patch"`
}

func paraminfo(plugin *C.LilvPlugin, world *C.LilvWorld) AllInfo {
        ports := PortsInfo(plugin, world)

        midis := MidiInfo(plugin, world)

        params := ParamsInfo(plugin, world)

        return AllInfo{ControlInput: ports, MidiParameter: midis, PatchParameter: params}
}


func PortsInfo(plugin *C.LilvPlugin, world *C.LilvWorld) []Info {
	var ports []Info

	toggle := C.lilv_new_uri(world, C.CString("http://lv2plug.in/ns/lv2core#toggled"))
	enum := C.lilv_new_uri(world, C.CString("http://lv2plug.in/ns/lv2core#enumeration"))
	defer func() {
		C.lilv_node_free(toggle)
		C.lilv_node_free(enum)
	}()

	numPorts := uint(C.lilv_plugin_get_num_ports(plugin))
	for i := uint(0); i < numPorts; i++ {
		port := C.lilv_plugin_get_port_by_index(plugin, C.uint(i))

		symbol := C.lilv_port_get_symbol(plugin, port)
		name := C.lilv_port_get_name(plugin, port)

		info := Info{
			Index:  fmt.Sprintf("%d",i),
			Symbol: C.GoString(C.lilv_node_as_string(symbol)),
			Name:   C.GoString(C.lilv_node_as_string(name)),
		}

		check := func(uri string) bool {
			curi := C.lilv_new_uri(world, C.CString(uri))
			defer C.lilv_node_free(curi)
			return C.lilv_port_is_a(plugin, port, curi) == true
		}

		if check("http://lv2plug.in/ns/lv2core#InputPort") {
			info.Input = true
		}
		if check("http://lv2plug.in/ns/lv2core#OutputPort") {
			info.Output = true
		}
		if check("http://lv2plug.in/ns/lv2core#AudioPort") {
			info.Audio = true
		}
		if check("http://lv2plug.in/ns/lv2core#ControlPort") {
			info.Control = true
		}
		if check("http://lv2plug.in/ns/ext/atom#AtomPort") {
			info.Atom = true
		}

		// Control port properties
		if info.Control {
			getProp := func(uri string) *C.LilvNode {
				curi := C.lilv_new_uri(world, C.CString(uri))
				defer C.lilv_node_free(curi)
				return C.lilv_port_get(plugin, port, curi)
			}

			if def := getProp("http://lv2plug.in/ns/lv2core#default"); def != nil {
				info.Default = float32(C.lilv_node_as_float(def))
			}
			if min := getProp("http://lv2plug.in/ns/lv2core#minimum"); min != nil {
				info.Min = float32(C.lilv_node_as_float(min))
			}
			if max := getProp("http://lv2plug.in/ns/lv2core#maximum"); max != nil {
				info.Max = float32(C.lilv_node_as_float(max))
			}

			// Display prio
			prioURI := C.CString("http://lv2plug.in/ns/ext/port-props#displayPriority")
			defer C.free(unsafe.Pointer(prioURI))
			prioNode := C.lilv_new_uri(world, prioURI)
			defer C.lilv_node_free(prioNode)

			if prio := C.lilv_port_get(plugin, port, prioNode); prio != nil {
				info.Prio = float32(C.lilv_node_as_float(prio))
			}

		       info.Toggle = C.lilv_port_has_property(plugin, port, toggle) == true
                       if info.Toggle {
                         info.Scale = []Point{Point{Label: "Off", Value: 0}, Point{Label: "On", Value: 1}}
                       }
		       info.Enum = C.lilv_port_has_property(plugin, port, toggle) == true


		}

		// Scale points
		scalePoints := C.lilv_port_get_scale_points(plugin, port)
		if scalePoints != nil {
			var scaleList []Point
			for i := C.lilv_scale_points_begin(scalePoints); !C.lilv_scale_points_is_end(scalePoints, i) == true; i = C.lilv_scale_points_next(scalePoints, i) {
				sp := C.lilv_scale_points_get(scalePoints, i)
				label := C.lilv_scale_point_get_label(sp)
				value := C.lilv_scale_point_get_value(sp)

				scaleList = append(scaleList, Point{Label: C.GoString(C.lilv_node_as_string(label)), Value: float32(C.lilv_node_as_float(value)),
				})
			}
			C.lilv_scale_points_free(scalePoints)
			info.Scale = scaleList
		}

		ports = append(ports, info)
	}

        return ports
}


func MidiInfo(plugin *C.LilvPlugin, world *C.LilvWorld) []Info {
	var result []Info

	midiParams := C.lilv_new_uri(world, C.CString("http://helander.network/lv2/elvira#midi_params"))
	midiCC := C.lilv_new_uri(world, C.CString("http://helander.network/lv2/elvira#midiCC"))
	lv2default := C.lilv_new_uri(world, C.CString("http://lv2plug.in/ns/lv2core#default"))
	lv2min := C.lilv_new_uri(world, C.CString("http://lv2plug.in/ns/lv2core#minimum"))
	lv2max := C.lilv_new_uri(world, C.CString("http://lv2plug.in/ns/lv2core#maximum"))
	displayPriority := C.lilv_new_uri(world, C.CString("http://lv2plug.in/ns/ext/port-props#displayPriority"))
	portProperty := C.lilv_new_uri(world, C.CString("http://lv2plug.in/ns/lv2core#portProperty"))
	toggle := C.lilv_new_uri(world, C.CString("http://lv2plug.in/ns/lv2core#toggled"))
	enum := C.lilv_new_uri(world, C.CString("http://lv2plug.in/ns/lv2core#enumeration"))

	scalePoint := C.lilv_new_uri(world, C.CString("http://lv2plug.in/ns/lv2core#scalePoint"))
	rdfValue := C.lilv_new_uri(world, C.CString("http://www.w3.org/1999/02/22-rdf-syntax-ns#value"))
	rdfsLabelPred := C.lilv_new_uri(world, C.CString("http://www.w3.org/2000/01/rdf-schema#label"))

	defer func() {
		C.lilv_node_free(midiParams)
		C.lilv_node_free(midiCC)
		C.lilv_node_free(lv2default)
		C.lilv_node_free(lv2min)
		C.lilv_node_free(lv2max)
		C.lilv_node_free(displayPriority)
		C.lilv_node_free(portProperty)
		C.lilv_node_free(toggle)
		C.lilv_node_free(enum)
		C.lilv_node_free(scalePoint)
		C.lilv_node_free(rdfValue)
		C.lilv_node_free(rdfsLabelPred)
	}()

	nodes := C.lilv_plugin_get_value(plugin, midiParams)
	if nodes == nil || C.lilv_nodes_size(nodes) == 0 {
		return []Info{}
	}

	for i := C.lilv_nodes_begin(nodes); !C.lilv_nodes_is_end(nodes, i) == true; i = C.lilv_nodes_next(nodes, i) {
		param := C.lilv_nodes_get(nodes, i)
		info := Info{
			Uri: C.GoString(C.lilv_node_as_uri(param)),
		}

		if cc := C.lilv_world_get(world, param, midiCC, nil); cc != nil {
			info.Midicc = C.GoString(C.lilv_node_as_string(cc))
		}

		if label := C.lilv_world_get(world, param, rdfsLabelPred, nil); label != nil {
			info.Name = C.GoString(C.lilv_node_as_string(label))
		}
		if val := C.lilv_world_get(world, param, lv2default, nil); val != nil {
			info.Default = float32(C.lilv_node_as_float(val))
		}
		if val := C.lilv_world_get(world, param, lv2min, nil); val != nil {
			info.Min = float32(C.lilv_node_as_float(val))
		}
		if val := C.lilv_world_get(world, param, lv2max, nil); val != nil {
			info.Max = float32(C.lilv_node_as_float(val))
		}
		if val := C.lilv_world_get(world, param, displayPriority, nil); val != nil {
			info.Prio = float32(C.lilv_node_as_float(val))
		} else {
			info.Prio = 0
		}

		info.Enum = C.lilv_world_ask(world, param, portProperty, enum) == true
		info.Toggle = C.lilv_world_ask(world, param, portProperty, toggle) == true

		scaleNodes := C.lilv_world_find_nodes(world, param, scalePoint, nil)
		var scales []Point
		for j := C.lilv_nodes_begin(scaleNodes); !C.lilv_nodes_is_end(scaleNodes, j) == true; j = C.lilv_nodes_next(scaleNodes, j) {
			sp := C.lilv_nodes_get(scaleNodes, j)
			values := C.lilv_world_find_nodes(world, sp, rdfValue, nil)
			labels := C.lilv_world_find_nodes(world, sp, rdfsLabelPred, nil)
			if C.lilv_nodes_size(values) > 0 && C.lilv_nodes_size(labels) > 0 {
				val := C.lilv_nodes_get_first(values)
				lab := C.lilv_nodes_get_first(labels)
				scales = append(scales, Point {
					Label: C.GoString(C.lilv_node_as_string(lab)),
					Value: float32(C.lilv_node_as_float(val)),
				})
				C.lilv_nodes_free(values)
				C.lilv_nodes_free(labels)
			}
		}
		info.Scale = scales
		result = append(result, info)
	}

        return result
}



func ParamsInfo(plugin *C.LilvPlugin, world *C.LilvWorld) []Info {
	var result []Info

	patchWritable := C.lilv_new_uri(world, C.CString("http://lv2plug.in/ns/ext/patch#writable"))
	defer C.lilv_node_free(patchWritable)

	writableNodes := C.lilv_plugin_get_value(plugin, patchWritable)
	if writableNodes == nil || C.lilv_nodes_size(writableNodes) == 0 {
		return []Info{}
	}

	lv2default := C.lilv_new_uri(world, C.CString("http://lv2plug.in/ns/lv2core#default"))
	lv2min := C.lilv_new_uri(world, C.CString("http://lv2plug.in/ns/lv2core#minimum"))
	lv2max := C.lilv_new_uri(world, C.CString("http://lv2plug.in/ns/lv2core#maximum"))
	displayPriority := C.lilv_new_uri(world, C.CString("http://lv2plug.in/ns/ext/port-props#displayPriority"))
	portProperty := C.lilv_new_uri(world, C.CString("http://lv2plug.in/ns/lv2core#portProperty"))
	rdfsRange := C.lilv_new_uri(world, C.CString("http://www.w3.org/2000/01/rdf-schema#range"))
	rdfsLabel := C.lilv_new_uri(world, C.CString("http://www.w3.org/2000/01/rdf-schema#label"))
	scalePoint := C.lilv_new_uri(world, C.CString("http://lv2plug.in/ns/lv2core#scalePoint"))
	rdfValue := C.lilv_new_uri(world, C.CString("http://www.w3.org/1999/02/22-rdf-syntax-ns#value"))
	rdfsLabelPred := C.lilv_new_uri(world, C.CString("http://www.w3.org/2000/01/rdf-schema#label"))
	toggle := C.lilv_new_uri(world, C.CString("http://lv2plug.in/ns/lv2core#toggled"))
	enum := C.lilv_new_uri(world, C.CString("http://lv2plug.in/ns/lv2core#enumeration"))

	defer func() {
		C.lilv_node_free(lv2default)
		C.lilv_node_free(lv2min)
		C.lilv_node_free(lv2max)
		C.lilv_node_free(displayPriority)
		C.lilv_node_free(portProperty)
		C.lilv_node_free(rdfsRange)
		C.lilv_node_free(rdfsLabel)
		C.lilv_node_free(scalePoint)
		C.lilv_node_free(rdfValue)
		C.lilv_node_free(rdfsLabelPred)
		C.lilv_node_free(toggle)
		C.lilv_node_free(enum)
	}()

	for i := C.lilv_nodes_begin(writableNodes); !C.lilv_nodes_is_end(writableNodes, i) == true; i = C.lilv_nodes_next(writableNodes, i) {
		node := C.lilv_nodes_get(writableNodes, i)

		info := Info{
			Uri: C.GoString(C.lilv_node_as_uri(node)),
		}

		rangeNode := C.lilv_world_get(world, node, rdfsRange, nil)
		if rangeNode != nil {
			info.Range = C.GoString(C.lilv_node_as_uri(rangeNode))
		} else {
			info.Range = "unknown"
		}

		labelNode := C.lilv_world_get(world, node, rdfsLabel, nil)
		if labelNode != nil {
			info.Name = C.GoString(C.lilv_node_as_string(labelNode))
		}

		if val := C.lilv_world_get(world, node, lv2default, nil); val != nil {
			info.Default = float32(C.lilv_node_as_float(val))
		}
		if val := C.lilv_world_get(world, node, lv2min, nil); val != nil {
			info.Min = float32(C.lilv_node_as_float(val))
		}
		if val := C.lilv_world_get(world, node, lv2max, nil); val != nil {
			info.Max = float32(C.lilv_node_as_float(val))
		}
		if val := C.lilv_world_get(world, node, displayPriority, nil); val != nil {
			info.Prio = float32(C.lilv_node_as_float(val))
		} else {
			info.Prio = 0
		}

		info.Enum = C.lilv_world_ask(world, node, portProperty, enum) == true
		info.Toggle = C.lilv_world_ask(world, node, portProperty, toggle) == true

		// Scale points
		scaleNodes := C.lilv_world_find_nodes(world, node, scalePoint, nil)
		var scales []Point

		for j := C.lilv_nodes_begin(scaleNodes); !C.lilv_nodes_is_end(scaleNodes, j) == true; j = C.lilv_nodes_next(scaleNodes, j) {
			bnode := C.lilv_nodes_get(scaleNodes, j)

			vals := C.lilv_world_find_nodes(world, bnode, rdfValue, nil)
			labels := C.lilv_world_find_nodes(world, bnode, rdfsLabelPred, nil)

			if C.lilv_nodes_size(vals) > 0 && C.lilv_nodes_size(labels) > 0 {
				value := C.lilv_nodes_get_first(vals)
				label := C.lilv_nodes_get_first(labels)

				scales = append(scales, Point {
					Label: C.GoString(C.lilv_node_as_string(label)),
					Value: float32(C.lilv_node_as_float(value)),
				})

				C.lilv_nodes_free(vals)
				C.lilv_nodes_free(labels)
			}
		}
		info.Scale = scales
		result = append(result, info)
	}

        return result
}


func GetAllParamInfo(pluginUri string) AllInfo {

	cURI := C.CString(pluginUri)
	defer C.free(unsafe.Pointer(cURI))

	world := C.lilv_world_new()
	defer C.lilv_world_free(world)

	C.lilv_world_load_all(world)

	pluginUriNode := C.lilv_new_uri(world, cURI)
	defer C.lilv_node_free(pluginUriNode)

	plugins := C.lilv_world_get_all_plugins(world)

	var plugin *C.LilvPlugin
	for iter := C.lilv_plugins_begin(plugins); !C.lilv_plugins_is_end(plugins, iter); iter = C.lilv_plugins_next(plugins, iter) {
		p := C.lilv_plugins_get(plugins, iter)
		pURI := C.lilv_plugin_get_uri(p)

		if C.GoString(C.lilv_node_as_uri(pURI)) == pluginUri {
			plugin = p
			break
		}
	}

	if plugin == nil {
		return AllInfo{}
	}

    return paraminfo(plugin,world)

}


// =====================================================================================================
// ParaminfoHandler
// =====================================================================================================
func ParaminfoHandler(w http.ResponseWriter, r *http.Request) {
    if r.Method != http.MethodGet {
        http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
        return
    }

    uriParam := r.URL.Query().Get("uri")
    if uriParam == "" {
        http.Error(w, "Missing 'uri' parameter", http.StatusBadRequest)
        return
    }


    json.NewEncoder(w).Encode(GetAllParamInfo(uriParam))
}


// =====================================================================================================
// Init
// =====================================================================================================
func init() {
        http.HandleFunc("/paraminfo", ParaminfoHandler)
}
