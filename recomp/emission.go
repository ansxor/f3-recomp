package main

type Emission struct {
	Files  []string
	Report *Fields
}

func (e Emission) Summary() *Fields {
	return e.Report.Without("fallback_pcs", "source_files")
}

type Coverage struct {
	Path   string
	Report *Fields
}

func (c Coverage) Summary() *Fields {
	summary, _ := c.Report.Get("summary")
	return NewFields("coverage", summary, "report", c.Path)
}
