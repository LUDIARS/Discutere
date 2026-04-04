import { BrowserRouter, Routes, Route } from "react-router-dom";
import { MachinaPage } from "./pages/MachinaPage";

export default function App() {
  return (
    <BrowserRouter>
      <Routes>
        <Route path="/" element={<MachinaPage />} />
      </Routes>
    </BrowserRouter>
  );
}
